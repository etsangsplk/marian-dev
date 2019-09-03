#include "marian.h"
#include "crow.h"
#include "translator/beam_search.h"
#include "translator/output_printer.h"
#include "common/timer.h"
#include "common/utils.h"
#include "api/elg.h"
#include "3rd_party/rapidjson/include/rapidjson/document.h"
#include "3rd_party/rapidjson/include/rapidjson/writer.h"
#include "3rd_party/rapidjson/include/rapidjson/stringbuffer.h"
#include "translation_service.h"
#include <sstream>
#include <cstdlib>
#include <cuda.h>
#include <driver_types.h>
#include <cuda_runtime.h>
#include "api/json_request_handler.h"
// #include "api/elg/json_request_handler.h"

class LogHandler : public crow::ILogHandler {
    public:
        void log(std::string msg, crow::LogLevel loglevel) override {
          if (loglevel == crow::LogLevel::DEBUG)
            LOG(debug,msg);
          else if (loglevel == crow::LogLevel::INFO)
            LOG(info,msg);
          else if (loglevel == crow::LogLevel::WARNING)
            LOG(warn,msg);
          else if (loglevel == crow::LogLevel::ERROR)
            LOG(error,msg);
          else if (loglevel == crow::LogLevel::CRITICAL)
            LOG(critical,msg);
        }
};

typedef marian::server::TranslationService<marian::BeamSearch> tservice_t;

class RequestHandler{
  const std::string gui_file_;

  std::string
  get(const crow::request& req) const{
    crow::mustache::context ctx;
    ctx["URL"] = req.get_header_value("Host");
    return crow::mustache::load(gui_file_).render(ctx);
  }

  virtual
  std::string
  post(const crow::request& req) const = 0;

protected:
  RequestHandler(const std::string gui_file)
    : gui_file_(gui_file){}

public:

  crow::response
  operator()(const crow::request& req) const{
    LOG(debug, "{} REQUEST: {}",
        req.method == "GET"_method ? "GET" : "POST",
        req.url);
    std::string body;
    if (req.method == "GET"_method){
      body = get(req);
    }
    else if (req.method == "POST"_method){
      body = post(req);
      LOG(debug, "RESPONSE: {}", body);
    }
    else{
      return crow::response(501);
    }
    auto res = crow::response(200, body);
    res.add_header("Access-Control-Allow-Origin", "*");
    res.add_header("Access-Control-Allow-Headers", "Content-Type");
    return res;
  }

  // The following is a wrapper function to accommodate the hack necessary
  // to deal with Google Chrome being 'smart' about URLs and automatically
  // adding a slash when it thinks the URL is a path to a directory and not
  // a file.
  // crow::response
  // operator()(const crow::request& req, const std::string& zilch) const{
  //   return (*this)(req);
  // }


};

class BergamotRequestHandler : public RequestHandler {

  marian::server::BergamotJsonRequestHandlerV1<tservice_t> process_;

  std::string
  post(const crow::request& req) const{
    auto payload_field = req.url_params.get("payload");
    auto options_field = req.url_params.get("options");
    // to be used lated, with multi-model engines
    // auto srclang = req.url_params.get("src");
    // auto trglang = req.url_params.get("trg");
    std::string payload = payload_field ? payload_field : "text";
    std::string t_opts = options_field ? options_field : "options";

    marian::Ptr<rapidjson::Document> D = process_(req.body, payload, t_opts);
    std::string response = marian::server::serialize(*D);
    return response;
  }
public:
  BergamotRequestHandler(tservice_t& service, const std::string gui_file)
    : RequestHandler(gui_file), process_(service){}
};

class ElgRequestHandler : public RequestHandler {
  marian::server::ElgJsonRequestHandlerV1<tservice_t> process_;
  std::string
  post(const crow::request& req) const {
    marian::Ptr<rapidjson::Document> D = process_(req.body.c_str());
    return marian::server::serialize(*D);
  }
public:
  ElgRequestHandler(tservice_t& service, const std::string gui_file)
    : RequestHandler(gui_file), process_(service){}
};


int main(int argc, char* argv[])
{
  using namespace marian;
  using namespace marian::server;
  ConfigParser cp(cli::mode::translation);
  cp.addOption<int>("--port,-p","Server Options", "server port",18080);
  cp.addOption<int>("--queue-timeout","Server Options",
                    "max wait time (in ms) for new data before an underfull "
                    "batch is launched",100);
  cp.addOption<size_t>("--max-workers","Server Options",
                       "Maximum number of worker threads to deploy when using CPU.",
                       std::thread::hardware_concurrency());
  cp.addOption<std::string>("--server-root","Server Options",
                            "server's document root directory","./rest");
  cp.addOption<std::string>("--ssplit-prefix-file","Server Options",
                            "File with nonbreaking prefixes for sentence splitting.");

  auto options = cp.parseOptions(argc, argv, true);
  auto service = New<tservice_t>(options);
  service->start();

  crow::SimpleApp app;
  std::string doc_root = options->get<std::string>("server-root");
  if (doc_root.back() == '/') doc_root.pop_back();
  crow::mustache::set_base(doc_root+"/ui");

  BergamotRequestHandler bergamot_handler(*service,"bergamot_api_v1.html");
  ElgRequestHandler elg_handler(*service,"elg_api_v1.html");

  // For some odd reason (probably a bug in crow), a GET
  // on a path not ending in a parameter specification
  // or slash results in a 404 (not found) error.
  // This is a hack to prevent that. Unfortunately
  CROW_ROUTE(app, "/api/bergamot/v1")
    .methods("POST"_method)(bergamot_handler);

  // Google Chrome automatically appends a slash to the path
  // ending in /v1 above.
  CROW_ROUTE(app, "/api/bergamot/v1/") // legacy path, deprecated ...
    .methods("GET"_method)(bergamot_handler);
  CROW_ROUTE(app, "/api/bergamot/demo.html")
    .methods("GET"_method)(bergamot_handler);


  CROW_ROUTE(app, "/api/elg/v1")
    .methods("POST"_method)
    ([service](const crow::request& req){
      rapidjson::Document D;
      D.Parse(req.body.c_str());
      if (!D.IsObject()){
        return crow::response(200,"Invalid Json");
      }

      LOG(debug, "REQUEST: {}", server::serialize(D));
      auto R = server::elg::translate_v1(*service,D);
      std::string response = server::serialize(*R);
      LOG(debug,"RESPONSE: {}", response);
      if (R->HasMember("failure")){
        auto res = crow::response(500,response);
        res.set_header("Content-Type","application/json");
        return res;
      }
      else{
        auto res = crow::response(200,response);
        res.set_header("Content-Type","application/json");
        return res;
      }
    });

  CROW_ROUTE(app, "/api/ug/v1")
    .methods("POST"_method)
    ([service](const crow::request& req){
      rapidjson::Document D;
      std::cerr << "MESSAGE BODY IS " << req.body << std::endl;
      D.Parse(req.body.c_str());
      if (!D.IsObject()) {
        return crow::response(500,"Invalid Json");
      }
      std::cerr << "PARSED: " << server::serialize(D) << std::endl;
      server::NodeTranslation<> job(&D,*service);
      job.finish(D.GetAllocator());
      std::string response = server::serialize(D);
      std::cerr << response << std::endl;
      return crow::response(response.c_str());
    });


  // route for serving the UI (templated)
  CROW_ROUTE(app, "/")
    ([](const crow::request& req){
      crow::mustache::context ctx;
      ctx["URL"] = req.get_header_value("Host");
      return crow::mustache::load("demo.html").render(ctx);
    });

  // // route for serving files
  // CROW_ROUTE(app, "/<string>")
  //   ([](const crow::request& req, std::string path){
  //     marian::filesystem::Path p(doc_root+path);
  //     if (!p.exists())
  //       return crow::response(404, path+" not found");
  //     ifstream(doc_root+path);


  // //     crow::mustache::context ctx;
  // //     ctx["url"] = req.raw_url;
  // //     std::cout << "URL " << req.get_header_value("Host") << req.url << std::endl;
  // //     auto page = crow::mustache::load("demo.html");
  // //     return page.render(ctx);

  //     if (marian::filesystem::exists(path))
  //       return crow::mustache::load(path).render();
  //     return page.render();
  //   });

  // enables all log

  app.loglevel(crow::LogLevel::WARNING);
  // app.loglevel(crow::LogLevel::DEBUG);

  LogHandler logger;
  crow::logger::setHandler(&logger);
  app.port(options->get<int>("port")).multithreaded().run();
}
