
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
#include <future>

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

    // Preview area
    Box* previewBox = new Box(HORIZONTAL);
    m_previewLabel = new Label("Ready to capture...");
    previewBox->addChild(m_previewLabel);
    mainBox->addChild(previewBox);
    
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
      m_abort = true;  // Signal worker to abort
      if (m_worker.joinable()) {
          // Give it a moment to finish, then detach if still running
          auto future = std::async(std::launch::async, [this]{ 
              if (m_worker.joinable()) m_worker.join(); 
          });
          if (future.wait_for(std::chrono::milliseconds(500)) == std::future_status::timeout) {
              m_worker.detach();  // Prevent UI freeze
          }
      }
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
  std::atomic<bool> m_abort{false};
  std::string m_response;
  std::string m_error;
  
  ui::Label* m_statusLabel = nullptr;
  Label* m_previewLabel = nullptr;

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
     
     // Update preview
     Doc* doc = m_context->activeDocument();
     if (doc && doc->sprite() && m_previewLabel) {
         int w = doc->sprite()->width();
         int h = doc->sprite()->height();
         m_previewLabel->setText("Captured: " + std::to_string(w) + "x" + std::to_string(h));
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
  
  std::string generatePaletteTable() {
    std::stringstream ss;
    Doc* doc = m_context->activeDocument();
    if (!doc || !doc->sprite()) return "{}";
    
    doc::Palette* palette = doc->sprite()->palette(doc->sprite()->frame(0));
    ss << "{";
    for (int i = 0; i < palette->size() && i < 16; i++) {
        doc::color_t c = palette->getEntry(i);
        int r = doc::rgba_getr(c);
        int g = doc::rgba_getg(c);
        int b = doc::rgba_getb(c);
        int a = doc::rgba_geta(c);
        ss << "[" << i << "]=Color{r=" << r << ",g=" << g << ",b=" << b << ",a=" << a << "},";
    }
    ss << "}";
    return ss.str();
  }

  void processRequestWorker(std::string userPrompt, std::string imgBase64) {
    if (m_abort) return;  // Check abort flag
    
    int w = 0, h = 0;
    if (m_context && m_context->activeDocument() && m_context->activeDocument()->sprite()) {
        w = m_context->activeDocument()->sprite()->width();
        h = m_context->activeDocument()->sprite()->height();
    }
    std::string sizeHint = (w > 0 && h > 0) ? "CANVAS SIZE: " + std::to_string(w) + "x" + std::to_string(h) + " pixels. " : "";
    
    // Generate palette table
    std::string paletteTable = generatePaletteTable();

    // Build optimized prompt with hex grid support
    std::string systemPrompt = "Context: You are Aseprite Assistant. Use Lua to script Aseprite.\n\n" + sizeHint + "\n\n"
        "CRITICAL LAYER SAFETY: Always start by creating a new layer:\n"
        "```lua\n"
        "local layer = app.activeSprite:newLayer()\n"
        "layer.name = 'AI Generation'\n"
        "app.activeLayer = layer\n"
        "```\n\n"
        "OPTIMIZED DRAWING METHOD - You have a helper function for efficient drawing:\n"
        "```lua\n"
        "-- drawHexGrid(startX, startY, width, hexString, palette)\n"
        "-- hexString: each character (0-F) is a palette index\n"
        "-- Example: \"0001112000011120\" draws a 4x4 grid\n"
        "```\n\n"
        "CURRENT PALETTE (use ONLY these indices 0-F):\n" + paletteTable + "\n\n"
        "AVAILABLE METHODS:\n"
        "1. PREFERRED: Use drawHexGrid() for efficient pixel-perfect drawing\n"
        "   - Generate a hex string where each char is a palette index\n"
        "   - Example: drawHexGrid(0, 0, 8, \"00112233...\", palette)\n"
        "2. FALLBACK: Use app.activeImage:drawPixel(x, y, palette[index]) ONLY if needed\n"
        "   - Always use palette[index], NEVER Color{r=...,g=...,b=...}\n"
        "3. ANIMATION: Create frames with sprite:newFrame() or sprite:newEmptyFrame()\n\n"
        "STYLE REQUIREMENTS:\n"
        "- Create PROFESSIONAL, HIGH-QUALITY pixel art\n"
        "- Use shading and lighting for depth (not flat colors)\n"
        "- Maintain coherent color palette usage\n"
        "- Ensure proper proportions for pixel art\n"
        "- NO stray pixels or noise\n\n"
        "ALWAYS end with `app.refresh()`\n\n"
        "User Request: " + userPrompt + "\n\nOutput MUST be a complete Lua code block in markdown format.";

    json11::Json reqBody = json11::Json::object {
      { "contents", json11::Json::array {
        json11::Json::object {
          { "parts", json11::Json::array {
             json11::Json::object { { "text", systemPrompt } },
             json11::Json::object { { "inline_data", json11::Json::object { { "mime_type", "image/png" }, { "data", imgBase64 } } } }
          }}
        }
      }}
    };
    
    if (m_abort) return;  // Check abort flag before API call
    
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
    
    if (m_abort) return;  // Check abort flag
    
    if (req.send(resp)) {
       if (!m_abort) m_response = ss.str();
    } else {
       if (!m_abort) m_error = "Network Error";
    }
    
    if (!m_abort) m_done = true;
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
           
           // Inject palette helper
           std::string paletteTable = generatePaletteTable();
           std::string helperCode = 
               "-- Helper function for efficient pixel drawing\n"
               "function drawHexGrid(startX, startY, width, hexString, palette)\n"
               "    local x = 0\n"
               "    local y = 0\n"
               "    for i = 1, #hexString do\n"
               "        local char = hexString:sub(i, i)\n"
               "        local colorIndex = tonumber(char, 16)\n"
               "        if colorIndex and palette[colorIndex] then\n"
               "            app.activeImage:drawPixel(startX + x, startY + y, palette[colorIndex])\n"
               "        end\n"
               "        x = x + 1\n"
               "        if x >= width then\n"
               "            x = 0\n"
               "            y = y + 1\n"
               "        end\n"
               "    end\n"
               "end\n\n"
               "-- Current palette\n"
               "local palette = " + paletteTable + "\n\n";
           
           app::script::Engine engine;
           engine.evalCode(helperCode + luaCode);
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
