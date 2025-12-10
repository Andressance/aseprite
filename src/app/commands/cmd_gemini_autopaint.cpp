
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "app/app.h"
#include "app/commands/command.h"
#include "app/context.h"
#include "app/context_access.h"
#include "app/script/engine.h"
#include "app/ui/main_window.h"
#include "app/ui/status_bar.h"
#include "app/file/file.h"
#include "doc/sprite.h"
#include "doc/image.h"
#include "net/http_request.h"
#include "net/http_response.h"
#include "net/http_headers.h"
#include "ui/ui.h"
#include "base/fs.h"
#include "json11.hpp"

#include <string>
#include <vector>
#include <fstream>
#include <sstream>

#include <cstdlib>

std::string get_gemini_key() {
  if (const char* env_p = std::getenv("GEMINI_API_KEY")) {
    return std::string(env_p);
  }
  std::ifstream env_file(".env");
  std::string line;
  while (std::getline(env_file, line)) {
    if (line.find("GEMINI_API_KEY=") == 0) {
      return line.substr(15);
    }
  }
  return "";
}

namespace app {

using namespace ui;

namespace {

static const std::string base64_chars =
             "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
             "abcdefghijklmnopqrstuvwxyz"
             "0123456789+/";

std::string base64_encode(unsigned char const* bytes_to_encode, unsigned int in_len) {
  std::string ret;
  int i = 0;
  int j = 0;
  unsigned char char_array_3[3];
  unsigned char char_array_4[4];

  while (in_len--) {
    char_array_3[i++] = *(bytes_to_encode++);
    if (i == 3) {
      char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
      char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
      char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
      char_array_4[3] = char_array_3[2] & 0x3f;

      for(i = 0; (i <4) ; i++)
        ret += base64_chars[char_array_4[i]];
      i = 0;
    }
  }

  if (i) {
    for(j = i; j < 3; j++)
      char_array_3[j] = '\0';

    char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
    char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
    char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
    char_array_4[3] = char_array_3[2] & 0x3f;

    for (j = 0; (j < i + 1); j++)
      ret += base64_chars[char_array_4[j]];

    while((i++ < 3))
      ret += '=';
  }

  return ret;
}

} // namespace



#include "ui/timer.h"
#include <thread>
#include <atomic>

class GeminiChatWindow : public Window {
public:
  GeminiChatWindow(Context* context) 
    : Window(Window::WithTitleBar, "Gemini Chat") 
    , m_context(context)
    , m_done(false) {
    
    // Main layout
    Box* mainBox = new Box(VERTICAL);
    
    // History View
    m_view = new View();
    m_historyBox = new Box(VERTICAL);
    m_view->attachToView(m_historyBox);
    m_view->setExpansive(true); // Fill available space
    mainBox->addChild(m_view);

    // Input Area
    Box* inputBox = new Box(HORIZONTAL);
    m_input = new Entry(1024, "");
    m_input->setExpansive(true);
    m_send = new Button("Send");
    
    inputBox->addChild(m_input);
    inputBox->addChild(m_send);
    mainBox->addChild(inputBox);

    addChild(mainBox);

    // Events
    m_send->Click.connect([this]{ onSend(); });
    
    // Timer
    m_timer = new ui::Timer(100, this);
    m_timer->Tick.connect([this]{ onTick(); });

    // Initial size
    setBounds(gfx::Rect(0, 0, 400, 300));
    centerWindow();
  }
  
  ~GeminiChatWindow() {
      if (m_worker.joinable())
          m_worker.join();
      delete m_timer;
  }

private:
  Context* m_context;
  View* m_view;
  Box* m_historyBox;
  Entry* m_input;
  Button* m_send;
  ui::Timer* m_timer;
  
  std::thread m_worker;
  std::atomic<bool> m_done;
  std::string m_response;
  std::string m_error;
  
  ui::Label* m_statusLabel = nullptr;

  ui::Label* addMessage(const std::string& role, const std::string& text) {
      ui::Box* row = new ui::Box(HORIZONTAL);
      ui::Label* label = new ui::Label(role + ": " + text);
      row->addChild(label);
      m_historyBox->addChild(row);
      m_view->updateView();
      // Scroll to bottom (simple hack, might need layout refresh)
      m_view->setViewScroll(gfx::Point(0, 99999)); 
      return label;
  }

  void onSend() {
     std::string userPrompt = m_input->text();
     if (userPrompt.empty()) return;

     addMessage("User", userPrompt);
     m_input->setText("");
     m_input->setEnabled(false);
     m_send->setEnabled(false);
     
     m_statusLabel = addMessage("System", "Thinking...");
     
     // Capture Image on Main Thread
     std::string imgBase64 = captureImage();
     if (imgBase64.empty()) {
         if (m_statusLabel) m_statusLabel->setText("System: Error capturing image.");
         m_input->setEnabled(true);
         m_send->setEnabled(true);
         return;
     }

     m_done = false;
     m_error.clear();
     m_response.clear();
     
     // Start Worker
     if (m_worker.joinable()) m_worker.join();
     m_worker = std::thread([this, userPrompt, imgBase64] {
         this->processRequestWorker(userPrompt, imgBase64);
     });
     
     m_timer->start();
  }
  
  void onTick() {
      if (m_done) {
          m_timer->stop();
          if (m_worker.joinable()) m_worker.join();
          
          m_input->setEnabled(true);
          m_send->setEnabled(true);
          
          if (!m_error.empty()) {
              if (m_statusLabel) m_statusLabel->setText("System: " + m_error);
          } else {
              handleResponse(m_response);
          }
           // Focus input
           m_input->requestFocus();
      }
  }

  std::string captureImage() {
    std::string tempFile = "gemini_temp.png"; 
    Doc* doc = m_context->activeDocument();
    if (!doc) return "";

    std::string oldFn = doc->filename();
    doc->setFilename(tempFile);
    int res = app::save_document(m_context, doc);
    doc->setFilename(oldFn);
    
    if (res != 0) return "";
    
    std::ifstream fs(tempFile, std::ios::binary);
    std::vector<unsigned char> buffer(std::istreambuf_iterator<char>(fs), {});
    return base64_encode(buffer.data(), buffer.size());
  }

  void processRequestWorker(std::string userPrompt, std::string imgBase64) {
    int w = 0, h = 0;
    if (m_context && m_context->activeDocument() && m_context->activeDocument()->sprite()) {
        w = m_context->activeDocument()->sprite()->width();
        h = m_context->activeDocument()->sprite()->height();
    }
    std::string sizeHint = (w > 0 && h > 0) ? "CANVAS SIZE: " + std::to_string(w) + "x" + std::to_string(h) + " pixels. " : "";

    json11::Json reqBody = json11::Json::object {
      { "contents", json11::Json::array {
        json11::Json::object {
          { "parts", json11::Json::array {
             json11::Json::object { { "text", "Context: You are Aseprite Assistant. Use Lua to script Aseprite.\n\n" + sizeHint + "\n\nAPI REFERENCE:\n1. DRAWING: Use `app.activeImage:drawPixel(x, y, Color{r=255,g=0,b=0,a=255})`. Iterate x,y manually. DO NOT use algorithms that require reading pixels unless necessary.\n2. ANIMATION: If the user asks for animation, YOU MUST CREATE FRAMES.\n   - `app.activeSprite:newFrame()` duplicates the current frame.\n   - `app.activeSprite:newEmptyFrame()` creates a blank frame.\n   - `app.activeFrame = frame` switches focus.\n\n   Example (4 Frame Animation):\n   ```lua\n   local sprite = app.activeSprite\n   for i=1,4 do\n     local frame = sprite:newFrame()\n     app.activeFrame = frame\n     local img = app.activeImage\n     -- Draw modification for this frame\n   end\n   ```\n3. STYLE: Create PROFESSIONAL, HIGH-QUALITY pixel art with depth and shading.\n   - Use a coherent color palette.\n   - Use shading/lighting to define shapes (do not just draw flat blobs).\n   - If drawing a character, ensure proper proportions for the canvas size.\n   - Avoid noise or stray pixels.\n4. DO NOT use `app.useTool`.\n5. ALWAYS end with `app.refresh()`.\n\nUser Request: " + userPrompt + "\nOutput MUST be a Markdown Lua code block." } },
             json11::Json::object { { "inline_data", json11::Json::object { { "mime_type", "image/png" }, { "data", imgBase64 } } } }
          }}
        }
      }}
    };
    
    std::string bodyFunc = reqBody.dump();
    
    std::string apiKey = get_gemini_key();
    if (apiKey.empty()) {
        m_error = "API Key not found in .env or environment";
        m_done = true;
        return;
    }
    
    net::HttpRequest req("https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent?key=" + apiKey);
    req.setBody(bodyFunc);
    
    net::HttpHeaders headers;
    headers.setHeader("Content-Type", "application/json");
    req.setHeaders(headers);
    
    std::stringstream ss;
    net::HttpResponse resp(&ss);
    
    if (req.send(resp)) {
       m_response = ss.str();
    } else {
       m_error = "Network Error";
    }
    m_done = true;
  }
  
  void handleResponse(const std::string& jsonStr) {
       std::string err;
       auto json = json11::Json::parse(jsonStr, err);
       if (!err.empty()) {
          if(m_statusLabel) m_statusLabel->setText("System: JSON Parse Error");
          return;
       }
       
       if (!json["error"].is_null()) {
           if(m_statusLabel) m_statusLabel->setText("System: API Error: " + json["error"]["message"].string_value());
           return;
       }
       
       if (json["candidates"].array_items().empty()) {
           if(m_statusLabel) m_statusLabel->setText("System: No candidates.");
           return;
       }
       
       std::string text = json["candidates"][0]["content"]["parts"][0]["text"].string_value();
       
       // Lua Extraction
       std::string luaCode;
       size_t start = text.find("```lua");
       if (start != std::string::npos) {
          start += 6;
          size_t end = text.find("```", start);
          if (end != std::string::npos) {
             luaCode = text.substr(start, end - start);
          }
       } else {
             start = text.find("```");
             if (start != std::string::npos) {
                start += 3;
                size_t end = text.find("```", start);
                if (end != std::string::npos) {
                  luaCode = text.substr(start, end - start);
                }
             }
       }
       
       if (!luaCode.empty()) {
          if(m_statusLabel) m_statusLabel->setText("System: Executing script...");
          app::script::Engine engine;
          engine.evalCode(luaCode);
          if(m_statusLabel) m_statusLabel->setText("System: Done.");
       } else {
          if(m_statusLabel) m_statusLabel->setText("System: No code found.");
          addMessage("Gemini", text.substr(0, 100) + "..."); // Preview
       }
  }
};

class GeminiAutopaintCommand : public Command {
public:
  GeminiAutopaintCommand() : Command("GeminiAutopaint") {}

protected:
  void onExecute(Context* context) override {
    GeminiChatWindow window(context);
    window.openWindowInForeground();
  }
};


// ... Register ...
Command* CommandFactory::createGeminiAutopaintCommand() {
  return new GeminiAutopaintCommand;
}

} // namespace app
