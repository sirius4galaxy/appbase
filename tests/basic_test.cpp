#include <appbase/application.hpp>
#include <iostream>
#include <string_view>
#include <thread>
#include <future>
#include <boost/exception/diagnostic_information.hpp>


#define BOOST_TEST_MODULE Basic Tests
#include <boost/test/included/unit_test.hpp>

namespace bu = boost::unit_test;
namespace bpo = boost::program_options;

using bpo::options_description;
using bpo::variables_map;
using std::string;
using std::vector;

class pluginA : public appbase::plugin<pluginA>
{
public:
   APPBASE_PLUGIN_REQUIRES();

   virtual void set_program_options( options_description& cli, options_description& cfg ) override {
      cli.add_options()
         ("readonly", "open db in read only mode")
         ("dbsize", bpo::value<uint64_t>()->default_value( 8*1024 ), "Minimum size MB of database shared memory file")
         ("replay", "clear db and replay all blocks" )
         ("log", "log messages" );
   }

   void plugin_initialize( const variables_map& options ) {
      readonly_ = !!options.count("readonly");
      replay_   = !!options.count("replay");
      log_      = !!options.count("log");
      dbsize_   = options.at("dbsize").as<uint64_t>();
      log("initialize pluginA");
   }
   
   void plugin_startup()  { log("starting pluginA"); }
   void plugin_shutdown() {
      log("shutdown pluginA");
      if (shutdown_counter)
         ++(*shutdown_counter);
   }
   
   uint64_t dbsize() const { return dbsize_; }
   bool     readonly() const { return readonly_; }
   
   void     do_throw(std::string msg) { throw std::runtime_error(msg); }
   void     set_shutdown_counter(uint32_t &c) { shutdown_counter = &c; }
   
   void     log(std::string_view s) const {
      if (log_)
         std::cout << s << "\n";
   }

private:
   bool      readonly_ {false};
   bool      replay_ {false};
   bool      log_ {false};
   uint64_t  dbsize_ {0};
   uint32_t* shutdown_counter { nullptr };
};

class pluginB : public appbase::plugin<pluginB>
{
public:
   pluginB(){};
   ~pluginB(){};

   APPBASE_PLUGIN_REQUIRES( (pluginA) );

   virtual void set_program_options( options_description& cli, options_description& cfg ) override {
      cli.add_options()
         ("endpoint", bpo::value<string>()->default_value( "127.0.0.1:9876" ), "address and port.")
         ("log2", "log messages" )
         ("throw", "throw an exception in plugin_shutdown()" )
         ;
   }

   void plugin_initialize( const variables_map& options ) {
      endpoint_ = options.at("endpoint").as<string>();
      log_      = !!options.count("log");
      throw_    = !!options.count("throw");
      log("initialize pluginB");
   }
   
   void plugin_startup()  { log("starting pluginB"); }
   void plugin_shutdown() {
      log("shutdown pluginB");
      if (shutdown_counter)
         ++(*shutdown_counter);
      if (throw_)
         do_throw("throwing in shutdown");
   }

   const string& endpoint() const { return endpoint_; }
   
   void     do_throw(std::string msg) { throw std::runtime_error(msg); }
   void     set_shutdown_counter(uint32_t &c) { shutdown_counter = &c; }
   
   void     log(std::string_view s) const {
      if (log_)
         std::cout << s << "\n";
   }
   
private:
   bool   log_ {false};
   bool   throw_ {false};
   string endpoint_;
   uint32_t* shutdown_counter { nullptr };
};


// -----------------------------------------------------------------------------
// Check that program options are correctly passed to plugins
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(program_options)
{
   appbase::application::register_plugin<pluginB>();

   appbase::scoped_app app;
   
   const char* argv[] = { bu::framework::current_test_case().p_name->c_str(),
                          "--plugin", "pluginA", "--readonly", "--replay", "--dbsize", "10000",
                          "--plugin", "pluginB", "--endpoint", "127.0.0.1:55", "--throw" };
   
   BOOST_CHECK(app->initialize<pluginB>(sizeof(argv) / sizeof(char*), const_cast<char**>(argv)));

   auto& pA = app->get_plugin<pluginA>();
   BOOST_CHECK(pA.dbsize() == 10000);
   BOOST_CHECK(pA.readonly());

   auto& pB = app->get_plugin<pluginB>();
   BOOST_CHECK(pB.endpoint() == "127.0.0.1:55");
}

// -----------------------------------------------------------------------------
// Check that configured plugins are started correctly
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(app_execution)
{
   appbase::application::register_plugin<pluginB>();

   appbase::scoped_app app;
   
   const char* argv[] = { bu::framework::current_test_case().p_name->c_str(),
                          "--plugin", "pluginA", "--log",
                          "--plugin", "pluginB", "--log2" };
   
   BOOST_CHECK(app->initialize<pluginB>(sizeof(argv) / sizeof(char*), const_cast<char**>(argv)));

   std::promise<std::tuple<pluginA&, pluginB&>> plugin_promise;
   std::future<std::tuple<pluginA&, pluginB&>> plugin_fut = plugin_promise.get_future();
   std::thread app_thread( [&]() {
      app->startup();
      plugin_promise.set_value( {app->get_plugin<pluginA>(), app->get_plugin<pluginB>()} );
      app->exec();
   } );

   auto [pA, pB] = plugin_fut.get();
   BOOST_CHECK(pA.get_state() == appbase::abstract_plugin::started);
   BOOST_CHECK(pB.get_state() == appbase::abstract_plugin::started);

   app->quit();
   app_thread.join();
}

// -----------------------------------------------------------------------------
// Check application lifetime managed by appbase::scoped_app
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(scoped_app_lifetime)
{
   appbase::application::register_plugin<pluginB>();

   {
      // create and run an `application` instance
      appbase::scoped_app app;
   
      const char* argv[] = { bu::framework::current_test_case().p_name->c_str() };
   
      BOOST_CHECK(app->initialize<pluginB>(sizeof(argv) / sizeof(char*), const_cast<char**>(argv)));

      std::promise<std::tuple<pluginA&, pluginB&>> plugin_promise;
      std::future<std::tuple<pluginA&, pluginB&>> plugin_fut = plugin_promise.get_future();
      std::thread app_thread( [&]() {
         app->startup();
         plugin_promise.set_value( {app->get_plugin<pluginA>(), app->get_plugin<pluginB>()} );
         app->exec();
      } );

      auto [pA, pB] = plugin_fut.get();
      BOOST_CHECK(pA.get_state() == appbase::abstract_plugin::started);
      BOOST_CHECK(pB.get_state() == appbase::abstract_plugin::started);

      std::cout << "Started first application instance\n";
      app->quit();
      app_thread.join();
   }

   {
      // create and run another `application` instance
      appbase::scoped_app app;
   
      const char* argv[] = { bu::framework::current_test_case().p_name->c_str() };
   
      BOOST_CHECK(app->initialize<pluginB>(sizeof(argv) / sizeof(char*), const_cast<char**>(argv)));

      std::promise<std::tuple<pluginA&, pluginB&>> plugin_promise;
      std::future<std::tuple<pluginA&, pluginB&>> plugin_fut = plugin_promise.get_future();
      std::thread app_thread( [&]() {
         app->startup();
         plugin_promise.set_value( {app->get_plugin<pluginA>(), app->get_plugin<pluginB>()} );
         app->exec();
      } );

      auto [pA, pB] = plugin_fut.get();
      BOOST_CHECK(pA.get_state() == appbase::abstract_plugin::started);
      BOOST_CHECK(pB.get_state() == appbase::abstract_plugin::started);

      std::cout << "Started second application instance\n";
      app->quit();
      app_thread.join();
   }
   
}

// -----------------------------------------------------------------------------
// Here we make sure that if the app gets an exeption in the `app().exec()` loop,
// 1. the exception is caught by the appbase framework, and logged
// 2. all plugins are shutdown (verified with shutdown_counter)
// 3. the exception is rethrown so the `main()` program can catch it if desired.
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(exception_in_exec)
{
   appbase::application::register_plugin<pluginB>();

   appbase::scoped_app app;
   
   const char* argv[] = { bu::framework::current_test_case().p_name->c_str(),
                          "--plugin", "pluginA", "--log",
                          "--plugin", "pluginB", "--log2" };
   
   BOOST_CHECK(app->initialize<pluginB>(sizeof(argv) / sizeof(char*), const_cast<char**>(argv)));

   std::promise<std::tuple<pluginA&, pluginB&>> plugin_promise;
   std::future<std::tuple<pluginA&, pluginB&>> plugin_fut = plugin_promise.get_future();
   std::thread app_thread( [&]() {
      app->startup();
      plugin_promise.set_value( {app->get_plugin<pluginA>(), app->get_plugin<pluginB>()} );
      try {
         app->exec();
      } catch(const std::exception& e ) {
         std::cout << "exception in exec (as expected): " << e.what() << "\n";
      }
   } );

   auto [pA, pB] = plugin_fut.get();
   BOOST_CHECK(pA.get_state() == appbase::abstract_plugin::started);
   BOOST_CHECK(pB.get_state() == appbase::abstract_plugin::started);

   uint32_t shutdown_counter = 0;
   pA.set_shutdown_counter(shutdown_counter);
   pB.set_shutdown_counter(shutdown_counter);
   
   std::this_thread::sleep_for(std::chrono::milliseconds(20));

   // this will throw an exception causing `app->exec()` to exit
   app->post(appbase::priority::high, [&] () { pA.do_throw("throwing in pluginA"); });
   
   app_thread.join();

   BOOST_CHECK(shutdown_counter == 2); // make sure both plugins shutdonn correctly
}

// -----------------------------------------------------------------------------
// Here we make sure that if the app gets an exeption in the `app().exec()` loop,
// 1. the exception is caught by the appbase framework, and logged
// 2. all plugins are shutdown (verified with shutdown_counter)
// 3. if the first plugin to be shutdown (pluginB) throws an exception, the second
//    plugin is still shutdown before the exception is rethrown.
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(exception_in_shutdown)
{
   appbase::application::register_plugin<pluginB>();

   appbase::scoped_app app;
   
   const char* argv[] = { bu::framework::current_test_case().p_name->c_str(),
                          "--plugin", "pluginA", "--log",
                          "--plugin", "pluginB", "--log2", "--throw" };
   
   BOOST_CHECK(app->initialize<pluginB>(sizeof(argv) / sizeof(char*), const_cast<char**>(argv)));

   std::promise<std::tuple<pluginA&, pluginB&>> plugin_promise;
   std::future<std::tuple<pluginA&, pluginB&>> plugin_fut = plugin_promise.get_future();
   std::thread app_thread( [&]() {
      app->startup();
      plugin_promise.set_value( {app->get_plugin<pluginA>(), app->get_plugin<pluginB>()} );
      try {
         app->exec();
      } catch(const std::exception& e ) {
         std::cout << "exception in exec (as expected): " << e.what() << "\n";
      }
   } );

   auto [pA, pB] = plugin_fut.get();
   BOOST_CHECK(pA.get_state() == appbase::abstract_plugin::started);
   BOOST_CHECK(pB.get_state() == appbase::abstract_plugin::started);

   uint32_t shutdown_counter = 0;
   pA.set_shutdown_counter(shutdown_counter);
   pB.set_shutdown_counter(shutdown_counter);
   
   std::this_thread::sleep_for(std::chrono::milliseconds(20));

   // this will throw an exception causing `app->exec()` to exit
   app->post(appbase::priority::high, [&] () { pA.do_throw("throwing in pluginA"); });
   
   app_thread.join();

   BOOST_CHECK(shutdown_counter == 2); // make sure both plugins shutdonn correctly,
                                       // even though there was a throw
}

// -----------------------------------------------------------------------------
// Make sure that queue is emptied when `app->quit()` is called, and that the
// queued tasks are *not* executed
// -----------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(queue_emptied_at_quit)
{
   appbase::application::register_plugin<pluginB>();

   appbase::scoped_app app;
   
   const char* argv[] = { bu::framework::current_test_case().p_name->c_str() };
   
   BOOST_CHECK(app->initialize<pluginB>(sizeof(argv) / sizeof(char*), const_cast<char**>(argv)));

   std::promise<std::tuple<pluginA&, pluginB&>> plugin_promise;
   std::future<std::tuple<pluginA&, pluginB&>> plugin_fut = plugin_promise.get_future();
   std::thread app_thread( [&]() {
      app->startup();
      plugin_promise.set_value( {app->get_plugin<pluginA>(), app->get_plugin<pluginB>()} );
      try {
         app->exec();
      } catch(const std::exception& e ) {
         std::cout << "exception in exec (as expected): " << e.what() << "\n";
      }
   } );

   auto [pA, pB] = plugin_fut.get();
   BOOST_CHECK(pA.get_state() == appbase::abstract_plugin::started);
   BOOST_CHECK(pB.get_state() == appbase::abstract_plugin::started);

   auto fib = [](uint64_t x) -> uint64_t {
      auto fib_impl = [](uint64_t n, auto& impl) -> uint64_t {
         return (n <= 1) ? n : impl(n - 1, impl) + impl(n - 2, impl);
      };
      return fib_impl(x, fib_impl);
   };

   uint32_t shutdown_counter = 0;
   pA.set_shutdown_counter(shutdown_counter);
   pB.set_shutdown_counter(shutdown_counter);
   
   uint64_t num_computed = 0, res;

   // computing 100 fib(32) takes about a second on my machine... so the app->quit() should
   // be processed while there are still plenty in the queue
   for (uint64_t i=0; i<100; ++i)
      app->post(appbase::priority::high, [&]() { res = fib(32); ++num_computed; });

   app->quit();
   
   app_thread.join();

   std::cout << "num_computed: " << num_computed << "\n";
   BOOST_CHECK(num_computed < 100);
   BOOST_CHECK(shutdown_counter == 2); // make sure both plugins shutdown correctly,
}


