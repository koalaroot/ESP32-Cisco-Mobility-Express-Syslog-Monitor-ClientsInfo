#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebServer.h>

// WiFi配置
const char* ssid = "<SSID>";
const char* password = "<PSK>";

// Bark配置
const char* bark_server = "api.day.app";
const char* bark_key = "<bark_api_key>";
const String bark_url_base = String("https://") + bark_server + "/" + bark_key;

// Web服务器认证信息
const char* web_username = "<admin>";
const char* web_password = "<password>";
const int web_port = 80;

// UDP配置
WiFiUDP udp;
const unsigned int udpPort = 514;
char packetBuffer[1024];

// Web服务器
WebServer server(web_port);

// 设备标识
const String device_name = "MobilityExpress-Syslog-Monitor";

// 每日重启配置
const int reboot_hour = 9;     // 每天重启的小时（24小时制）
const int reboot_minute = 0;   // 重启的分钟
bool has_rebooted_today = false; // 标记今天是否已经重启过
unsigned long last_time_check = 0; // 上次检查时间的时间戳

// 客户端信息结构体
struct ClientInfo {
    String username;
    String mac_address;
    time_t first_seen;      // 首次发现时间
    time_t last_seen;       // 最后在线时间
    time_t offline_time;    // 离线时间
    bool is_online;         // 当前是否在线
    int connection_count;   // 连接次数
    bool never_online;      // 从未在线过（只收到过离线消息）
    
    // 构造函数
    ClientInfo() {
        username = "";
        mac_address = "";
        is_online = false;
        connection_count = 0;
        first_seen = 0;
        last_seen = 0;
        offline_time = 0;
        never_online = false;
    }
    
    // 获取在线时长（秒）
    long getOnlineDuration() const {
        if (is_online) {
            return time(nullptr) - last_seen;
        }
        if (offline_time > 0 && last_seen > 0 && !never_online) {
            return offline_time - last_seen;
        }
        return 0;
    }
    
    // 获取格式化时间（静态方法）
    static String getFormattedTime(time_t t) {
        if (t == 0) return "从未上线";
        struct tm timeinfo;
        localtime_r(&t, &timeinfo);
        char buffer[20];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
        return String(buffer);
    }
    
    // 获取格式化在线时长
    String getFormattedDuration() const {
        if (never_online) return "从未在线";
        
        long duration = getOnlineDuration();
        if (duration <= 0) return "0秒";
        
        long hours = duration / 3600;
        long minutes = (duration % 3600) / 60;
        long seconds = duration % 60;
        
        String result = "";
        if (hours > 0) result += String(hours) + "小时";
        if (minutes > 0) result += String(minutes) + "分钟";
        if (seconds > 0 || result.length() == 0) result += String(seconds) + "秒";
        
        return result;
    }
    
    // 更新在线状态
    void updateOnline() {
        time_t now = time(nullptr);
        if (!is_online) {
            if (first_seen == 0) first_seen = now;
            if (never_online) never_online = false; // 如果曾经在线过，清除从未在线标志
            connection_count++;
            is_online = true;
            Serial.println("客户端上线: " + mac_address + " (" + username + ")");
        }
        last_seen = now;
        offline_time = 0;
    }
    
    // 更新离线状态
    void updateOffline(bool is_new_client = false) {
        time_t now = time(nullptr);
        if (is_online || is_new_client) {
            if (is_new_client) {
                // 新客户端，只收到离线消息
                never_online = true;
                first_seen = 0; // 从未上线，所以首次上线时间为0
                last_seen = 0;  // 从未上线，所以最后上线时间为0
                connection_count = 0; // 从未连接过
                Serial.println("创建从未在线的客户端: " + mac_address + " (" + username + ")");
            }
            
            is_online = false;
            offline_time = now;
            Serial.println("客户端离线: " + mac_address + " (" + username + ") 离线时间: " + ClientInfo::getFormattedTime(offline_time));
            
            if (is_new_client) {
                Serial.println("注意: 此客户端从未被检测到上线，仅收到离线消息");
            }
        }
    }
    
    // 检查MAC地址是否匹配
    bool matches(const String& mac) const {
        return mac_address.equalsIgnoreCase(mac);
    }
};

// 使用链表结构
struct ClientNode {
    ClientInfo client;
    ClientNode* next;
    
    ClientNode() : next(nullptr) {}
};

// 全局变量
ClientNode* clientsHead = nullptr;
int clientCount = 0;
SemaphoreHandle_t clientsMutex = NULL;
const int MAX_CLIENTS = 200;

// 用户信息结构体
struct UserInfo {
    String username;
    String mac_address;
    bool has_username;
    bool has_mac;
};

// 初始化客户端列表
bool initClientsList() {
    clientsMutex = xSemaphoreCreateMutex();
    if (clientsMutex == NULL) {
        Serial.println("✗ 创建互斥锁失败");
        return false;
    }
    
    clientsHead = nullptr;
    clientCount = 0;
    
    Serial.println("✓ 客户端链表初始化成功");
    return true;
}

// 查找客户端节点
ClientNode* findClientNode(const String& mac_address) {
    String mac_upper = mac_address;
    mac_upper.toUpperCase();
    
    ClientNode* current = clientsHead;
    while (current != nullptr) {
        if (current->client.matches(mac_upper)) {
            return current;
        }
        current = current->next;
    }
    return nullptr;
}

// 添加新客户端（从未在线过的情况）
ClientNode* addNewOfflineClient(const String& username, const String& mac_address) {
    if (clientCount >= MAX_CLIENTS) {
        Serial.println("✗ 已达到最大客户端数量限制: " + String(MAX_CLIENTS));
        return nullptr;
    }
    
    String mac_upper = mac_address;
    mac_upper.toUpperCase();
    
    ClientNode* newNode = new ClientNode();
    if (newNode == nullptr) {
        Serial.println("✗ 内存分配失败");
        return nullptr;
    }
    
    newNode->client.username = username;
    newNode->client.mac_address = mac_upper;
    newNode->client.first_seen = 0;          // 从未上线
    newNode->client.last_seen = 0;           // 从未上线
    newNode->client.offline_time = time(nullptr); // 当前时间作为离线时间
    newNode->client.is_online = false;       // 离线状态
    newNode->client.connection_count = 0;    // 从未连接过
    newNode->client.never_online = true;     // 标记为从未在线
    
    // 插入到链表头部
    newNode->next = clientsHead;
    clientsHead = newNode;
    clientCount++;
    
    Serial.println("添加从未在线的客户端: " + mac_upper + " (" + username + ")");
    return newNode;
}

// 添加新客户端（正常上线情况）
ClientNode* addNewOnlineClient(const String& username, const String& mac_address) {
    if (clientCount >= MAX_CLIENTS) {
        Serial.println("✗ 已达到最大客户端数量限制: " + String(MAX_CLIENTS));
        return nullptr;
    }
    
    String mac_upper = mac_address;
    mac_upper.toUpperCase();
    
    ClientNode* newNode = new ClientNode();
    if (newNode == nullptr) {
        Serial.println("✗ 内存分配失败");
        return nullptr;
    }
    
    newNode->client.username = username;
    newNode->client.mac_address = mac_upper;
    newNode->client.first_seen = time(nullptr);
    newNode->client.connection_count = 1;
    newNode->client.is_online = true;
    newNode->client.last_seen = time(nullptr);
    newNode->client.offline_time = 0;
    newNode->client.never_online = false;    // 正常在线客户端
    
    // 插入到链表头部
    newNode->next = clientsHead;
    clientsHead = newNode;
    clientCount++;
    
    Serial.println("添加新客户端: " + mac_upper + " (" + username + ")");
    return newNode;
}

// 发送Bark通知函数
void sendBarkNotification(String title, String content, String group = "MobilityExpress Monitor", String sound = "bell") {
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    HTTPClient http;
    http.begin(bark_url_base);
    http.addHeader("Content-Type", "application/json");
    
    DynamicJsonDocument doc(1024);
    doc["title"] = title;
    doc["body"] = content;
    doc["group"] = group;
    doc["sound"] = sound;
    
    String jsonString;
    serializeJson(doc, jsonString);
    
    http.POST(jsonString);
    http.end();
}

// 清理和格式化消息
String cleanMessage(String message) {
    message.trim();
    if (message.length() > 500) {
        message = message.substring(0, 500) + "...[truncated]";
    }
    return message;
}

// 改进的MAC地址提取函数
String extractMacAddress(String message) {
    String mac = "";
    
    // 尝试多种模式匹配MAC地址
    int patterns[] = {
        message.indexOf("for mobile "),
        message.indexOf("the mobile "),
        message.indexOf("mobile "),
        message.indexOf("MAC: "),
        message.indexOf("mac: ")
    };
    
    for (int i = 0; i < 5; i++) {
        if (patterns[i] != -1) {
            int startPos = patterns[i];
            
            // 根据不同的模式调整起始位置
            if (i == 0) startPos += 11; // "for mobile ".length()
            else if (i == 1) startPos += 11; // "the mobile ".length()
            else if (i == 2) startPos += 7; // "mobile ".length()
            else if (i == 3) startPos += 5; // "MAC: ".length()
            else if (i == 4) startPos += 5; // "mac: ".length()
            
            String remaining = message.substring(startPos);
            
            // 提取MAC地址（支持xx:xx:xx:xx:xx:xx或xx-xx-xx-xx-xx-xx格式）
            int endPos = -1;
            int colonCount = 0;
            int dashCount = 0;
            
            for (int j = 0; j < remaining.length() && j < 17; j++) {
                char c = remaining.charAt(j);
                if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
                    // 合法MAC字符
                } else if (c == ':' || c == '-') {
                    if (c == ':') colonCount++;
                    else dashCount++;
                } else {
                    endPos = j;
                    break;
                }
            }
            
            if (endPos == -1 && (remaining.length() >= 12)) {
                endPos = (remaining.length() > 17) ? 17 : remaining.length();
            }
            
            if (endPos > 0) {
                mac = remaining.substring(0, endPos);
                // 验证MAC地址格式
                if ((colonCount == 5 || dashCount == 5) && mac.length() == 17) {
                    mac.toUpperCase();
                    Serial.println("提取到MAC地址: " + mac);
                    return mac;
                }
            }
        }
    }
    
    return mac;
}

// 改进的用户名提取函数
String extractUsername(String message) {
    String username = "";
    
    // 尝试多种模式
    int entryPos = message.indexOf("entry (");
    if (entryPos != -1) {
        int startPos = entryPos + 7; // "entry (".length()
        int endPos = message.indexOf(")", startPos);
        if (endPos != -1) {
            username = message.substring(startPos, endPos);
            Serial.println("提取到用户名: " + username);
        }
    }
    
    // 如果没找到，尝试其他模式
    if (username.length() == 0) {
        int userPos = message.indexOf("user ");
        if (userPos != -1) {
            int startPos = userPos + 5;
            int endPos = message.indexOf(" ", startPos);
            if (endPos == -1) endPos = message.indexOf("\n", startPos);
            if (endPos == -1) endPos = message.length();
            
            if (endPos > startPos) {
                username = message.substring(startPos, endPos);
                Serial.println("提取到用户名(备用): " + username);
            }
        }
    }
    
    return username;
}

// 提取用户名和MAC地址
UserInfo extractUserInfo(String message) {
    UserInfo info;
    info.has_username = false;
    info.has_mac = false;
    
    // 提取用户名
    info.username = extractUsername(message);
    if (info.username.length() > 0) {
        info.has_username = true;
    }
    
    // 提取MAC地址
    info.mac_address = extractMacAddress(message);
    if (info.mac_address.length() > 0) {
        info.has_mac = true;
    }
    
    // 调试信息
    if (info.has_mac) {
        Serial.println("解析结果 - MAC: " + info.mac_address + ", 用户: " + info.username);
    }
    
    return info;
}

// 获取当前时间
String getCurrentTime() {
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
        return "时间未同步";
    }
    
    char timeString[20];
    strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &timeinfo);
    return String(timeString);
}

// 获取当前日期字符串（用于判断是否是新的一天）
String getCurrentDate() {
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
        return "";
    }
    
    char dateString[11];
    strftime(dateString, sizeof(dateString), "%Y-%m-%d", &timeinfo);
    return String(dateString);
}

// 检查是否需要每日重启
void checkDailyReboot() {
    static String last_checked_date = "";
    static unsigned long last_check_time = 0;
    
    // 每30秒检查一次，避免频繁检查
    if (millis() - last_check_time < 30000) {
        return;
    }
    
    last_check_time = millis();
    
    struct tm timeinfo;
    if(!getLocalTime(&timeinfo)){
        Serial.println("无法获取时间，跳过重启检查");
        return;
    }
    
    String current_date = getCurrentDate();
    if (current_date.length() == 0) {
        return;
    }
    
    // 如果是新的一天，重置重启标记
    if (last_checked_date != current_date) {
        has_rebooted_today = false;
        last_checked_date = current_date;
        Serial.println("新的一天: " + current_date + "，重置重启标记");
    }
    
    // 检查是否到达重启时间（上午9:00）
    if (timeinfo.tm_hour == reboot_hour && 
        timeinfo.tm_min == reboot_minute && 
        timeinfo.tm_sec < 30) { // 30秒窗口内执行重启
        
        if (!has_rebooted_today) {
            Serial.println("╔════════════════════════════════════════╗");
            Serial.println("║     到达每日重启时间 (09:00)           ║");
            Serial.println("║     准备重启ESP32...                   ║");
            Serial.println("╚════════════════════════════════════════╝");
            
            // 发送重启通知
            if (WiFi.status() == WL_CONNECTED) {
                String ipAddress = WiFi.localIP().toString();
                String currentTime = getCurrentTime();
                
                sendBarkNotification(
                    "🔁 ESP32每日重启",
                    String("设备: ") + device_name + "\n" +
                    "IP地址: " + ipAddress + "\n" +
                    "重启时间: " + currentTime + "\n" +
                    "原因: 计划每日维护重启\n" +
                    "设备将在30秒后重新启动...",
                    "System Maintenance",
                    "alarm"
                );
            }
            
            // 标记已重启
            has_rebooted_today = true;
            
            // 等待30秒，确保通知发送完成，并给用户时间查看
            Serial.println("等待30秒后重启...");
            for (int i = 30; i > 0; i--) {
                Serial.println("倒计时: " + String(i) + " 秒");
                delay(1000);
            }
            
            // 执行重启
            Serial.println("正在重启ESP32...");
            ESP.restart();
        }
    }
}

// 更新客户端状态
void updateClientStatus(const String& username, const String& mac_address, bool is_online) {
    if (mac_address.length() == 0) {
        Serial.println("错误: MAC地址为空，无法更新状态");
        return;
    }
    
    if (clientsMutex && xSemaphoreTake(clientsMutex, portMAX_DELAY) == pdTRUE) {
        ClientNode* node = findClientNode(mac_address);
        
        if (node == nullptr) {
            // 新客户端
            if (is_online) {
                // 收到上线消息，创建正常客户端
                node = addNewOnlineClient(username, mac_address);
                if (node) {
                    Serial.println("新客户端已添加（上线状态）: " + mac_address);
                }
            } else {
                // 收到离线消息，但客户端不存在，创建从未在线的客户端
                node = addNewOfflineClient(username, mac_address);
                if (node) {
                    Serial.println("新客户端已添加（从未在线）: " + mac_address);
                }
            }
        } else {
            // 现有客户端
            String old_status = node->client.is_online ? "在线" : "离线";
            String new_status = is_online ? "在线" : "离线";
            
            if (is_online) {
                node->client.updateOnline();
                if (username.length() > 0) {
                    node->client.username = username;
                }
            } else {
                // 如果是从未在线的客户端，保持这个标记
                bool was_never_online = node->client.never_online;
                node->client.updateOffline(false); // 不是新客户端
                
                // 如果之前是"从未在线"状态，收到离线消息时不改变这个状态
                if (was_never_online) {
                    node->client.never_online = true;
                }
            }
            
            if (old_status != new_status) {
                Serial.println("客户端状态变化: " + mac_address + " 从 " + old_status + " 变为 " + new_status);
            }
        }
        
        xSemaphoreGive(clientsMutex);
    }
}

// 解析日志并提取信息
void parseSyslogMessage(String message) {
    String msgLower = message;
    msgLower.toLowerCase();
    String cleanedMessage = cleanMessage(message);
    
    Serial.println("\n=== 收到系统日志 ===");
    Serial.println(cleanedMessage);
    
    UserInfo info = extractUserInfo(message);
    
    bool is_online_event = false;
    bool is_offline_event = false;
    
    // 检查是否为用户上线消息
    if (msgLower.indexOf("user_name_created") != -1 || 
        (msgLower.indexOf("username entry") != -1 &&
         msgLower.indexOf("created for mobile") != -1) ||
        msgLower.indexOf("successful login") != -1 ||
        msgLower.indexOf("user authenticated") != -1 ||
        msgLower.indexOf("connected") != -1) {
        
        Serial.println("=== 检测到用户上线事件 ===");
        is_online_event = true;
        
        updateClientStatus(info.username, info.mac_address, true);
        
        String title = "MobilityExpress-entry-created";
        String content = cleanedMessage + "\n\n";
        
        if (info.has_username) {
            content += "用户名: " + info.username + "\n";
        }
        
        if (info.has_mac) {
            content += "MAC地址: " + info.mac_address + "\n";
        }
        
        content += "时间: " + getCurrentTime();
        
        sendBarkNotification(title, content, "MobilityExpress Monitor", "bell");
    }
    
    // 检查是否为用户离线消息（USER_NAME_DELETED）
    else if (msgLower.indexOf("user_name_deleted") != -1 || 
             (msgLower.indexOf("username entry") != -1 &&
              msgLower.indexOf("deleted for mobile") != -1)) {
        
        Serial.println("=== 检测到用户离线事件 (USER_NAME_DELETED) ===");
        is_offline_event = true;
        
        updateClientStatus(info.username, info.mac_address, false);
        
        String title = "MobilityExpress-entry-deleted";
        String content = cleanedMessage + "\n\n";
        
        if (info.has_username) {
            content += "用户名: " + info.username + "\n";
        }
        
        if (info.has_mac) {
            content += "MAC地址: " + info.mac_address + "\n";
        }
        
        content += "时间: " + getCurrentTime();
        
        sendBarkNotification(title, content, "MobilityExpress Monitor", "glass");
    }
    
    // 检查是否为用户离线消息（MOBILESTATION_NOT_FOUND）
    else if (msgLower.indexOf("mobilestation_not_found") != -1 || 
             msgLower.indexOf("could not find the mobile") != -1) {
        
        Serial.println("=== 检测到用户离线事件 (MOBILESTATION_NOT_FOUND) ===");
        is_offline_event = true;
        
        updateClientStatus(info.username, info.mac_address, false);
        
        String title = "MobilityExpress-entry Could not find";
        String content = cleanedMessage + "\n\n";
        
        if (info.has_username) {
            content += "用户名: " + info.username + "\n";
        }
        
        if (info.has_mac) {
            content += "MAC地址: " + info.mac_address + "\n";
        }
        
        content += "时间: " + getCurrentTime();
        
        sendBarkNotification(title, content, "MobilityExpress Monitor", "glass");
    }
    
    // 其他可能的离线消息
    else if (msgLower.indexOf("disconnected") != -1 || 
             msgLower.indexOf("logout") != -1 ||
             msgLower.indexOf("offline") != -1 ||
             msgLower.indexOf("session terminated") != -1 ||
             msgLower.indexOf("connection closed") != -1) {
        
        Serial.println("=== 检测到可能的离线事件 ===");
        is_offline_event = true;
        
        // 仍然尝试提取信息并标记为离线
        if (info.has_mac) {
            updateClientStatus(info.username, info.mac_address, false);
            Serial.println("标记为离线: " + info.mac_address);
        }
    }
    
    // 如果没有明确的事件类型，根据关键词判断
    if (!is_online_event && !is_offline_event && info.has_mac) {
        // 如果有MAC地址但没有明确事件，暂时不处理，等待明确事件
        Serial.println("有MAC地址但没有明确事件类型: " + info.mac_address);
    }
    
    // 打印当前客户端状态
    if (clientsMutex && xSemaphoreTake(clientsMutex, portMAX_DELAY) == pdTRUE) {
        Serial.println("当前客户端数量: " + String(clientCount));
        ClientNode* current = clientsHead;
        while (current != nullptr) {
            String status = current->client.is_online ? "在线" : "离线";
            if (current->client.never_online) {
                status = "离线(从未在线)";
            }
            Serial.println("  " + current->client.mac_address + 
                          " - " + status +
                          " - 用户: " + current->client.username);
            current = current->next;
        }
        xSemaphoreGive(clientsMutex);
    }
}

// WiFi连接函数
void connectToWiFi() {
    Serial.println("连接WiFi: " + String(ssid));
    
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);
    WiFi.begin(ssid, password);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n✓ WiFi连接成功!");
        Serial.println("IP地址: " + WiFi.localIP().toString());
    } else {
        Serial.println("\n✗ WiFi连接失败");
    }
}

// 初始化时间同步
void initTime() {
    configTime(8 * 3600, 0, "ntp.aliyun.com", "ntp1.aliyun.com", "pool.ntp.org");
    
    Serial.println("等待时间同步...");
    int attempts = 0;
    while (time(nullptr) < 1000000000 && attempts < 30) {
        Serial.print(".");
        delay(1000);
        attempts++;
    }
    
    struct tm timeinfo;
    if(getLocalTime(&timeinfo)){
        Serial.println("\n✓ 时间同步成功");
        
        // 打印当前时间
        char timeString[20];
        strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &timeinfo);
        Serial.println("当前时间: " + String(timeString));
        
        // 打印每日重启时间
        Serial.println("每日重启时间: " + String(reboot_hour) + ":" + 
                      (reboot_minute < 10 ? "0" : "") + String(reboot_minute));
    } else {
        Serial.println("\n✗ 时间同步失败");
    }
}

// Web认证中间件
bool checkWebAuthentication() {
    if (!server.authenticate(web_username, web_password)) {
        server.requestAuthentication();
        return false;
    }
    return true;
}

// 生成HTML页面
void handleRoot() {
    if (!checkWebAuthentication()) return;
    
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html; charset=utf-8", "");
    
    // 开始HTML
    server.sendContent("<!DOCTYPE html><html lang='zh-CN'><head>");
    server.sendContent("<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>");
    server.sendContent("<title>MobilityExpress 客户端状态监控</title>");
    server.sendContent("<style>");
    server.sendContent("body { font-family: Arial, sans-serif; margin: 10px; background-color: #f5f5f5; font-size: 14px; }");
    server.sendContent(".container { max-width: 100%; margin: 0 auto; }");
    server.sendContent("h1 { color: #333; text-align: center; font-size: 20px; margin: 10px 0; }");
    server.sendContent(".stats { background: white; padding: 10px; border-radius: 5px; margin-bottom: 15px; box-shadow: 0 1px 3px rgba(0,0,0,0.1); }");
    server.sendContent(".client-table { width: 100%; border-collapse: collapse; margin-top: 15px; font-size: 12px; }");
    server.sendContent(".client-table th, .client-table td { border: 1px solid #ddd; padding: 6px; text-align: left; }");
    server.sendContent(".client-table th { background-color: #4CAF50; color: white; }");
    server.sendContent(".client-table tr:nth-child(even) { background-color: #f9f9f9; }");
    server.sendContent(".client-table tr:hover { background-color: #f5f5f5; }");
    server.sendContent(".online { color: green; font-weight: bold; }");
    server.sendContent(".offline { color: red; font-weight: bold; }");
    server.sendContent(".never-online { color: #888; font-style: italic; }");
    server.sendContent(".status-badge { padding: 2px 6px; border-radius: 10px; font-size: 11px; }");
    server.sendContent(".online-badge { background-color: #d4edda; color: #155724; }");
    server.sendContent(".offline-badge { background-color: #f8d7da; color: #721c24; }");
    server.sendContent(".never-online-badge { background-color: #e2e3e5; color: #383d41; }");
    server.sendContent(".header { display: flex; flex-wrap: wrap; justify-content: space-between; align-items: center; margin-bottom: 15px; }");
    server.sendContent(".btn { background-color: #4CAF50; color: white; padding: 8px 15px; border: none; border-radius: 4px; cursor: pointer; margin: 2px; font-size: 13px; }");
    server.sendContent(".btn:hover { opacity: 0.9; }");
    server.sendContent(".btn-danger { background-color: #f44336; }");
    server.sendContent(".btn-warning { background-color: #ff9800; }");
    server.sendContent(".btn-group { display: flex; flex-wrap: wrap; }");
    server.sendContent("@media (max-width: 768px) { .header { flex-direction: column; } .btn-group { margin-top: 10px; } }");
    server.sendContent(".info-box { background: #e7f3fe; border-left: 4px solid #2196F3; padding: 10px; margin: 10px 0; }");
    server.sendContent("</style></head><body><div class='container'>");
    
    // 头部
    server.sendContent("<div class='header'>");
    server.sendContent("<h1>📡 MobilityExpress ClientsInfo</h1>");
    server.sendContent("<div class='btn-group'>");
    server.sendContent("<button class='btn' onclick='location.reload()'>🔄 刷新</button>");
    server.sendContent("<button class='btn btn-warning' onclick='if(confirm(\"确定要立即重启设备吗？\")) location.href=\"/reboot\"'>🔁 立即重启</button>");
    server.sendContent("<button class='btn btn-danger' onclick='if(confirm(\"清空所有数据？\")) location.href=\"/clear\"'>🗑️ 清空</button>");
    server.sendContent("</div></div>");
    
    // 信息框 - 每日重启信息
    server.sendContent("<div class='info-box'>");
    server.sendContent("<strong>📅 每日重启功能</strong><br>");
    server.sendContent("重启时间: 每天 " + String(reboot_hour) + ":" + 
                      (reboot_minute < 10 ? "0" : "") + String(reboot_minute) + "<br>");
    
    // 修复字符串连接问题
    String rebootStatus = "今日已重启: ";
    rebootStatus += (has_rebooted_today ? "是" : "否");
    rebootStatus += "<br>";
    rebootStatus += "下次重启: ";
    rebootStatus += (has_rebooted_today ? "明天" : "今天");
    rebootStatus += " ";
    rebootStatus += String(reboot_hour);
    rebootStatus += ":";
    if (reboot_minute < 10) rebootStatus += "0";
    rebootStatus += String(reboot_minute);
    
    server.sendContent(rebootStatus);
    server.sendContent("</div>");
    
    // 统计信息
    int online_count = 0;
    int offline_never_online_count = 0;
    int offline_normal_count = 0;
    int total_count = 0;
    
    if (clientsMutex && xSemaphoreTake(clientsMutex, portMAX_DELAY) == pdTRUE) {
        total_count = clientCount;
        ClientNode* current = clientsHead;
        while (current != nullptr) {
            if (current->client.is_online) {
                online_count++;
            } else if (current->client.never_online) {
                offline_never_online_count++;
            } else {
                offline_normal_count++;
            }
            current = current->next;
        }
        xSemaphoreGive(clientsMutex);
    }
    
    server.sendContent("<div class='stats'>");
    server.sendContent("<h3>📊 统计信息</h3>");
    server.sendContent("<p>在线: <span class='online'>" + String(online_count) + "</span> | ");
    server.sendContent("离线(曾在线): <span class='offline'>" + String(offline_normal_count) + "</span> | ");
    server.sendContent("离线(从未在线): <span class='never-online'>" + String(offline_never_online_count) + "</span></p>");
    server.sendContent("<p>总计: " + String(total_count) + "</p>");
    server.sendContent("<p>时间: " + getCurrentTime() + "</p>");
    server.sendContent("<p>IP: " + WiFi.localIP().toString() + " | 内存: " + String(esp_get_free_heap_size() / 1024) + "KB</p>");
    server.sendContent("</div>");
    
    // 在线客户端表格
    server.sendContent("<h2>🟢 Online Clients (" + String(online_count) + ")</h2>");
    if (online_count > 0) {
        server.sendContent("<table class='client-table'><tr>");
        server.sendContent("<th>用户名</th><th>MAC地址</th><th>首次上线</th><th>最后上线</th><th>在线时长</th><th>连接次数</th><th>状态</th>");
        server.sendContent("</tr>");
        
        if (clientsMutex && xSemaphoreTake(clientsMutex, portMAX_DELAY) == pdTRUE) {
            ClientNode* current = clientsHead;
            while (current != nullptr) {
                const ClientInfo& client = current->client;
                if (client.is_online) {
                    server.sendContent("<tr>");
                    server.sendContent("<td>" + (client.username.length() > 0 ? client.username : "-") + "</td>");
                    server.sendContent("<td>" + client.mac_address + "</td>");
                    server.sendContent("<td>" + ClientInfo::getFormattedTime(client.first_seen) + "</td>");
                    server.sendContent("<td>" + ClientInfo::getFormattedTime(client.last_seen) + "</td>");
                    server.sendContent("<td>" + client.getFormattedDuration() + "</td>");
                    server.sendContent("<td>" + String(client.connection_count) + "</td>");
                    server.sendContent("<td><span class='status-badge online-badge'>在线</span></td>");
                    server.sendContent("</tr>");
                }
                current = current->next;
            }
            xSemaphoreGive(clientsMutex);
        }
        
        server.sendContent("</table>");
    } else {
        server.sendContent("<p style='color: #666; text-align: center;'>暂无在线客户端</p>");
    }
    
    // 离线客户端表格（曾在线）
    server.sendContent("<h2>🔴 Offline Clients - 曾在线 (" + String(offline_normal_count) + ")</h2>");
    if (offline_normal_count > 0) {
        server.sendContent("<table class='client-table'><tr>");
        server.sendContent("<th>用户名</th><th>MAC地址</th><th>首次上线</th><th>最后上线</th><th>离线时间</th><th>最后在线时长</th><th>连接次数</th><th>状态</th>");
        server.sendContent("</tr>");
        
        if (clientsMutex && xSemaphoreTake(clientsMutex, portMAX_DELAY) == pdTRUE) {
            ClientNode* current = clientsHead;
            while (current != nullptr) {
                const ClientInfo& client = current->client;
                if (!client.is_online && !client.never_online) {
                    server.sendContent("<tr>");
                    server.sendContent("<td>" + (client.username.length() > 0 ? client.username : "-") + "</td>");
                    server.sendContent("<td>" + client.mac_address + "</td>");
                    server.sendContent("<td>" + ClientInfo::getFormattedTime(client.first_seen) + "</td>");
                    server.sendContent("<td>" + ClientInfo::getFormattedTime(client.last_seen) + "</td>");
                    server.sendContent("<td>" + ClientInfo::getFormattedTime(client.offline_time) + "</td>");
                    server.sendContent("<td>" + client.getFormattedDuration() + "</td>");
                    server.sendContent("<td>" + String(client.connection_count) + "</td>");
                    server.sendContent("<td><span class='status-badge offline-badge'>离线</span></td>");
                    server.sendContent("</tr>");
                }
                current = current->next;
            }
            xSemaphoreGive(clientsMutex);
        }
        
        server.sendContent("</table>");
    } else {
        server.sendContent("<p style='color: #666; text-align: center;'>暂无曾在线离线记录</p>");
    }
    
    // 离线客户端表格（从未在线）
    server.sendContent("<h2>⚫ Offline Clients - 从未在线 (" + String(offline_never_online_count) + ")</h2>");
    if (offline_never_online_count > 0) {
        server.sendContent("<table class='client-table'><tr>");
        server.sendContent("<th>用户名</th><th>MAC地址</th><th>首次发现</th><th>离线时间</th><th>状态说明</th>");
        server.sendContent("</tr>");
        
        if (clientsMutex && xSemaphoreTake(clientsMutex, portMAX_DELAY) == pdTRUE) {
            ClientNode* current = clientsHead;
            while (current != nullptr) {
                const ClientInfo& client = current->client;
                if (!client.is_online && client.never_online) {
                    server.sendContent("<tr>");
                    server.sendContent("<td>" + (client.username.length() > 0 ? client.username : "-") + "</td>");
                    server.sendContent("<td>" + client.mac_address + "</td>");
                    server.sendContent("<td>" + ClientInfo::getFormattedTime(client.offline_time) + "</td>");
                    server.sendContent("<td>" + ClientInfo::getFormattedTime(client.offline_time) + "</td>");
                    server.sendContent("<td><span class='status-badge never-online-badge'>从未在线</span></td>");
                    server.sendContent("</tr>");
                }
                current = current->next;
            }
            xSemaphoreGive(clientsMutex);
        }
        
        server.sendContent("</table>");
    } else {
        server.sendContent("<p style='color: #666; text-align: center;'>暂无从未在线记录</p>");
    }
    
    // 页脚
    server.sendContent("<div style='margin-top: 20px; text-align: center; color: #666; font-size: 12px;'>");
    server.sendContent("<p>最后更新: " + getCurrentTime() + "</p>");
    
    // 修复页脚中的字符串连接
    String footerInfo = "<p>" + device_name + " | 版本: 2.4 (每日重启功能)</p>";
    footerInfo += "<p>每日重启时间: " + String(reboot_hour) + ":" + 
                 (reboot_minute < 10 ? "0" : "") + String(reboot_minute) + 
                 " | 今日已重启: " + (has_rebooted_today ? "是" : "否") + "</p>";
    
    server.sendContent(footerInfo);
    server.sendContent("<p>说明: 每日重启有助于清理内存，保持系统稳定运行</p>");
    server.sendContent("</div></div></body></html>");
    
    server.sendContent(""); // 结束内容
}

// JSON API接口
void handleAPI() {
    if (!checkWebAuthentication()) return;
    
    DynamicJsonDocument doc(2048);
    JsonObject stats = doc.createNestedObject("stats");
    
    if (clientsMutex && xSemaphoreTake(clientsMutex, portMAX_DELAY) == pdTRUE) {
        int online_count = 0;
        int offline_never_online_count = 0;
        int offline_normal_count = 0;
        int total_count = clientCount;
        
        ClientNode* current = clientsHead;
        while (current != nullptr) {
            if (current->client.is_online) {
                online_count++;
            } else if (current->client.never_online) {
                offline_never_online_count++;
            } else {
                offline_normal_count++;
            }
            current = current->next;
        }
        
        stats["online"] = online_count;
        stats["offline_normal"] = offline_normal_count;
        stats["offline_never_online"] = offline_never_online_count;
        stats["total"] = total_count;
        stats["max_clients"] = MAX_CLIENTS;
        stats["time"] = getCurrentTime();
        stats["ip"] = WiFi.localIP().toString();
        stats["free_heap"] = esp_get_free_heap_size();
        
        xSemaphoreGive(clientsMutex);
    }
    
    String jsonString;
    serializeJson(doc, jsonString);
    server.send(200, "application/json", jsonString);
}

// 清理所有客户端数据
void handleClear() {
    if (!checkWebAuthentication()) return;
    
    if (clientsMutex && xSemaphoreTake(clientsMutex, portMAX_DELAY) == pdTRUE) {
        // 释放所有节点内存
        ClientNode* current = clientsHead;
        while (current != nullptr) {
            ClientNode* next = current->next;
            delete current;
            current = next;
        }
        clientsHead = nullptr;
        clientCount = 0;
        xSemaphoreGive(clientsMutex);
        
        Serial.println("已清空所有客户端数据");
    }
    
    // 重定向回首页
    server.sendHeader("Location", "/");
    server.send(303);
}

// 立即重启设备
void handleReboot() {
    if (!checkWebAuthentication()) return;
    
    Serial.println("╔════════════════════════════════════════╗");
    Serial.println("║     手动触发设备重启                   ║");
    Serial.println("║     设备将在5秒后重启...               ║");
    Serial.println("╚════════════════════════════════════════╝");
    
    // 发送重启通知
    if (WiFi.status() == WL_CONNECTED) {
        String ipAddress = WiFi.localIP().toString();
        String currentTime = getCurrentTime();
        
        sendBarkNotification(
            "🔁 ESP32手动重启",
            String("设备: ") + device_name + "\n" +
            "IP地址: " + ipAddress + "\n" +
            "重启时间: " + currentTime + "\n" +
            "原因: 手动触发重启\n" +
            "设备将在5秒后重新启动...",
            "System Maintenance",
            "alarm"
        );
    }
    
    // 返回重启页面
    server.send(200, "text/html", 
        "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>重启中</title>"
        "<meta http-equiv='refresh' content='5;url=/'>"
        "<style>body{font-family:Arial,sans-serif;text-align:center;padding:50px;}</style>"
        "</head><body>"
        "<h1>设备重启中...</h1>"
        "<p>设备将在5秒后重启，并自动返回主页。</p>"
        "<p>如果长时间没有返回，请手动刷新页面。</p>"
        "</body></html>");
    
    // 延迟5秒后重启
    delay(5000);
    ESP.restart();
}

// 初始化Web服务器
void initWebServer() {
    server.on("/", handleRoot);
    server.on("/api", handleAPI);
    server.on("/clear", handleClear);
    server.on("/reboot", handleReboot);
    
    server.begin();
    Serial.println("✓ Web服务器已启动");
    Serial.println("  访问地址: http://" + WiFi.localIP().toString());
    Serial.println("  用户名: " + String(web_username));
    Serial.println("  密码: " + String(web_password));
    Serial.println("  每日重启时间: " + String(reboot_hour) + ":" + 
                  (reboot_minute < 10 ? "0" : "") + String(reboot_minute));
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n========================================");
    Serial.println("      MobilityExpress Syslog Monitor ESP32");
    Serial.println("              带Web Dashboard");
    Serial.println("========================================");
    Serial.println("设备: " + device_name);
    Serial.println("最大客户端数: " + String(MAX_CLIENTS));
    Serial.println("Bark Key: " + String(bark_key));
    Serial.println("Web认证: " + String(web_username) + "/" + String(web_password));
    Serial.println("每日重启时间: " + String(reboot_hour) + ":" + 
                  (reboot_minute < 10 ? "0" : "") + String(reboot_minute));
    Serial.println("========================================");
    
    // 初始化客户端链表
    if (!initClientsList()) {
        Serial.println("✗ 系统初始化失败");
        return;
    }
    
    // 连接WiFi
    connectToWiFi();
    
    // 初始化时间
    initTime();
    
    // 启动UDP监听
    if (udp.begin(udpPort)) {
        Serial.println("✓ UDP监听端口: " + String(udpPort));
    } else {
        Serial.println("✗ UDP启动失败");
    }
    
    // 初始化Web服务器
    initWebServer();
    
    // 发送启动通知
    if (WiFi.status() == WL_CONNECTED) {
        String ipAddress = WiFi.localIP().toString();
        String startTime = getCurrentTime();
        
        sendBarkNotification(
            "MobilityExpress Monitor Started",
            String("设备: ") + device_name + "\n" +
            "IP地址: " + ipAddress + "\n" +
            "启动时间: " + startTime + "\n" +
            "Web界面: http://" + ipAddress + "\n" +
            "每日重启: " + String(reboot_hour) + ":" + 
            (reboot_minute < 10 ? "0" : "") + String(reboot_minute) + "\n" +
            "状态: 监听514端口，Web服务已启动",
            "System Status",
            "calypso"
        );
        
        Serial.println("\n✓ 系统初始化完成");
        Serial.println("✓ 等待系统日志消息...");
        Serial.println("✓ 可用内存: " + String(esp_get_free_heap_size() / 1024) + "KB");
        Serial.println("✓ 每日重启功能已启用");
    }
    
    Serial.println("========================================");
}

void loop() {
    // 检查WiFi连接
    static unsigned long lastWifiCheck = 0;
    if (millis() - lastWifiCheck > 10000) { // 每10秒检查一次
        lastWifiCheck = millis();
        
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("WiFi断开连接，尝试重新连接...");
            connectToWiFi();
        }
    }
    
    // 处理客户端请求
    server.handleClient();
    
    // 处理UDP数据包
    int packetSize = udp.parsePacket();
    if (packetSize) {
        int len = udp.read(packetBuffer, sizeof(packetBuffer) - 1);
        if (len > 0) {
            packetBuffer[len] = '\0';
            parseSyslogMessage(String(packetBuffer));
        }
    }
    
    // 检查是否需要每日重启
    checkDailyReboot();
    
    delay(10);
}
