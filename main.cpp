#define SFML_STATIC
#include <SFML/Graphics.hpp>
#include <vector>
#include <string>
#include <iostream>
#include <windows.h>
#include <Xinput.h>
#include <thread>
#include <queue>
#include <mutex>
#include <cmath>  // 添加数学库头文件

#pragma comment(lib, "Xinput.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Winmm.lib")  // 添加Winmm库用于声音控制

using namespace std;
using namespace sf;

// 全局变量
std::queue<WORD> keyQueue;
std::mutex queueMutex;
bool keepSending = true;
bool shouldExit = false;

// 独立发送线程
void keySenderThread() {
    while (keepSending) {
        if (!keyQueue.empty()) {
            WORD keyData; {
                std::lock_guard<std::mutex> lock(queueMutex);
                keyData = keyQueue.front();
                keyQueue.pop();
            }

            INPUT input = {0};
            input.type = INPUT_KEYBOARD;

            // 检查是否是释放按键（高位为1）
            if (keyData & 0x8000) {
                // 释放按键
                input.ki.wVk = keyData & 0x7FFF;
                input.ki.dwFlags = KEYEVENTF_KEYUP;
            } else {
                // 按下按键
                input.ki.wVk = keyData;
                input.ki.dwFlags = 0;
            }

            // 发送按键
            SendInput(1, &input, sizeof(INPUT));
        }
        Sleep(5);
    }
}

// 自定义窗口过程 - 关键修复
LRESULT CALLBACK KeyboardWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static bool isClosing = false;

    switch (msg) {
        case WM_ACTIVATE:
        case WM_ACTIVATEAPP:
        case WM_NCACTIVATE:
        case WM_MOUSEACTIVATE:
            // 拦截所有激活消息，返回-1确保窗口不会激活
            return MA_NOACTIVATE;

        case WM_SETFOCUS:
            // 立即失去焦点
            SetFocus(NULL);
            return 0;

        case WM_WINDOWPOSCHANGING:
            // 防止窗口被带到前台
            if (!isClosing) {
                ((WINDOWPOS *) lParam)->flags |= SWP_NOACTIVATE;
            }
            break;

        case WM_CLOSE:
            isClosing = true;
            break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// 键盘按键类
class Key {
public:
    RectangleShape shape;
    string text;
    bool isSpecial;
    Text textObj;
    Color originalColor;

    Key(float x, float y, float width, float height, const string &txt, bool special = false, Font *font = nullptr)
        : text(txt), isSpecial(special) {
        shape.setPosition(x, y);
        shape.setSize(Vector2f(width, height));
        originalColor = Color::White;
        shape.setFillColor(originalColor);
        shape.setOutlineColor(Color::Black);
        shape.setOutlineThickness(2);

        if (font) {
            textObj.setFont(*font);
            textObj.setString(txt);
            textObj.setCharacterSize(16);
            textObj.setFillColor(Color::Black);

            FloatRect textRect = textObj.getLocalBounds();
            textObj.setOrigin(textRect.left + textRect.width / 2.0f,
                              textRect.top + textRect.height / 2.0f);
            textObj.setPosition(x + width / 2.0f, y + height / 2.0f);
        }
    }

    void draw(RenderWindow *window) {
        window->draw(shape);
        window->draw(textObj);
    }
};

class VirtualKeyboard {
private:
    RenderWindow window;
    Font font;
    vector<vector<Key> > keys;
    string inputText;
    int currentRow = 0;
    int currentCol = 0;
    bool joystickConnected = false;
    Clock inputClock;
    Clock buttonPressClock;
    Clock redrawClock;
    Clock selectionClock; // 新增：选择高亮控制
    Clock focusCheckClock; // 新增：焦点检查时钟
    bool needsRedraw = true;
    bool buttonPressed = false;
    XINPUT_STATE state;
    XINPUT_STATE prevState; // 新增：上一次状态

    // 新增状态变量
    bool isUpperCase = true; // 当前是否为大写
    bool isMinimized = false; // 窗口是否已最小化
    Clock comboKeyClock;

    // 窗口移动相关
    Clock moveClock; // 移动延迟控制
    Vector2i windowPosition; // 窗口位置

    // 新增：退出相关
    Clock exitClock;
    bool exitComboPressed = false;

    // 新增：焦点保持相关
    HWND gameWindowHandle = NULL;
    Clock gameWindowCheckClock;
    bool wasMinimized = false;

    void initFont() {
        // 尝试多种字体路径
        vector<string> fontPaths = {
            "C:/Windows/Fonts/arial.ttf",
            "C:/Windows/Fonts/msyh.ttc", // 微软雅黑
            "C:/Windows/Fonts/tahoma.ttf"
        };

        bool fontLoaded = false;
        for (const auto &path: fontPaths) {
            if (font.loadFromFile(path)) {
                fontLoaded = true;
                break;
            }
        }

        if (!fontLoaded) {
            // 如果都失败，使用默认字体
            cout << "Warning: Using default font" << endl;
        }
    }

    void initKeyboard() {
        float keyWidth = 60;
        float keyHeight = 50;
        float startX = 50;
        float startY = 100;
        float spacing = 5;

        // 数字行
        vector<Key> row1;
        for (int i = 0; i < 10; i++) {
            row1.push_back(Key(startX + i * (keyWidth + spacing),
                               startY, keyWidth, keyHeight, to_string(i), false, &font));
        }
        keys.push_back(row1);

        // 字母行 - 初始为大写
        const vector<string> row2Text = {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"};
        vector<Key> row2;
        for (int i = 0; i < row2Text.size(); i++) {
            row2.push_back(Key(startX + i * (keyWidth + spacing),
                               startY + keyHeight + spacing, keyWidth, keyHeight, row2Text[i], false, &font));
        }
        keys.push_back(row2);

        // 第三行
        const vector<string> row3Text = {"A", "S", "D", "F", "G", "H", "J", "K", "L", ";"};
        vector<Key> row3;
        for (int i = 0; i < row3Text.size(); i++) {
            row3.push_back(Key(startX + i * (keyWidth + spacing),
                               startY + 2 * (keyHeight + spacing), keyWidth, keyHeight, row3Text[i], false, &font));
        }
        keys.push_back(row3);

        // 第四行
        const vector<string> row4Text = {"Z", "X", "C", "V", "B", "N", "M", ",", ".", "/"};
        vector<Key> row4;
        for (int i = 0; i < row4Text.size(); i++) {
            row4.push_back(Key(startX + i * (keyWidth + spacing),
                               startY + 3 * (keyHeight + spacing), keyWidth, keyHeight, row4Text[i], false, &font));
        }
        keys.push_back(row4);

        // 特殊键行
        vector<Key> row5;
        row5.push_back(Key(startX, startY + 4 * (keyHeight + spacing),
                           keyWidth * 2, keyHeight, "SPACE", true, &font));
        row5.push_back(Key(startX + (keyWidth + spacing) * 2,
                           startY + 4 * (keyHeight + spacing),
                           keyWidth * 1.5, keyHeight, "BACK", true, &font));
        row5.push_back(Key(startX + (keyWidth + spacing) * 3.5,
                           startY + 4 * (keyHeight + spacing),
                           keyWidth * 1.5, keyHeight, "CLEAR", true, &font));
        row5.push_back(Key(startX + (keyWidth + spacing) * 5,
                           startY + 4 * (keyHeight + spacing),
                           keyWidth * 1.5, keyHeight, "ENTER", true, &font));
        row5.push_back(Key(startX + (keyWidth + spacing) * 6.5,
                           startY + 4 * (keyHeight + spacing),
                           keyWidth * 1.5, keyHeight, "EXIT", true, &font));
        keys.push_back(row5);
    }

    void updateKeyboardCase() {
        // 更新字母键的显示文本（大写/小写）
        if (isUpperCase) {
            // 第二行字母
            vector<string> upperRow2 = {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"};
            for (int i = 0; i < upperRow2.size(); i++) {
                keys[1][i].text = upperRow2[i];
                keys[1][i].textObj.setString(upperRow2[i]);
            }

            // 第三行字母
            vector<string> upperRow3 = {"A", "S", "D", "F", "G", "H", "J", "K", "L", ";"};
            for (int i = 0; i < upperRow3.size(); i++) {
                keys[2][i].text = upperRow3[i];
                keys[2][i].textObj.setString(upperRow3[i]);
            }

            // 第四行字母
            vector<string> upperRow4 = {"Z", "X", "C", "V", "B", "N", "M", ",", ".", "/"};
            for (int i = 0; i < upperRow4.size(); i++) {
                keys[3][i].text = upperRow4[i];
                keys[3][i].textObj.setString(upperRow4[i]);
            }
        } else {
            // 第二行字母
            vector<string> lowerRow2 = {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"};
            for (int i = 0; i < lowerRow2.size(); i++) {
                keys[1][i].text = lowerRow2[i];
                keys[1][i].textObj.setString(lowerRow2[i]);
            }

            // 第三行字母
            vector<string> lowerRow3 = {"a", "s", "d", "f", "g", "h", "j", "k", "l", ";"};
            for (int i = 0; i < lowerRow3.size(); i++) {
                keys[2][i].text = lowerRow3[i];
                keys[2][i].textObj.setString(lowerRow3[i]);
            }

            // 第四行字母
            vector<string> lowerRow4 = {"z", "x", "c", "v", "b", "n", "m", ",", ".", "/"};
            for (int i = 0; i < lowerRow4.size(); i++) {
                keys[3][i].text = lowerRow4[i];
                keys[3][i].textObj.setString(lowerRow4[i]);
            }
        }
        needsRedraw = true;
    }

    // 修正窗口样式设置 - 关键修复
    void makeWindowNonFocusable() {
        HWND hwnd = window.getSystemHandle();
        if (!hwnd) return;

        // 获取当前样式
        LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
        LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);

        // 移除可能引起问题的样式
        style &= ~WS_CAPTION;
        style &= ~WS_THICKFRAME;
        style &= ~WS_MINIMIZEBOX;
        style &= ~WS_MAXIMIZEBOX;
        style &= ~WS_SYSMENU;
        style |= WS_POPUP;
        style |= WS_CLIPCHILDREN;
        style |= WS_CLIPSIBLINGS;

        // 设置扩展样式
        exStyle = WS_EX_TOOLWINDOW | // 工具窗口，不在任务栏显示
                  WS_EX_NOACTIVATE | // 不会被激活
                  WS_EX_TOPMOST | // 始终置顶
                  WS_EX_LAYERED | // 分层窗口，避免闪烁
                  WS_EX_TRANSPARENT; // 透明，不接收鼠标消息

        // 应用样式
        SetWindowLongPtr(hwnd, GWL_STYLE, style);
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);

        // 设置分层窗口属性
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

        // 设置窗口过程
        SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR) KeyboardWindowProc);

        // 应用窗口位置和大小
        RECT rect;
        GetWindowRect(hwnd, &rect);
        SetWindowPos(hwnd, HWND_TOPMOST,
                     rect.left, rect.top,
                     rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);

        // 强制窗口失去焦点
        SetFocus(NULL);

        // 保存窗口位置
        windowPosition = Vector2i(rect.left, rect.top);

        cout << "Window set as non-focusable tool window" << endl;
    }

    // 重新应用窗口样式
    void reapplyWindowStyle() {
        HWND hwnd = window.getSystemHandle();
        if (!hwnd) return;

        // 短暂延迟
        Sleep(100);

        // 重新应用样式
        makeWindowNonFocusable();

        cout << "Window style reapplied" << endl;
    }

    // 移动窗口函数（使用十字键）
    void moveWindowWithDPad() {
        if (!joystickConnected || moveClock.getElapsedTime().asMilliseconds() < 100)
            return;

        bool moved = false;
        int moveDistance = 10; // 每次移动10像素

        // 上方向键
        if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_UP) {
            windowPosition.y -= moveDistance;
            moved = true;
        }
        // 下方向键
        if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) {
            windowPosition.y += moveDistance;
            moved = true;
        }
        // 左方向键
        if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) {
            windowPosition.x -= moveDistance;
            moved = true;
        }
        // 右方向键
        if (state.Gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) {
            windowPosition.x += moveDistance;
            moved = true;
        }

        if (moved) {
            // 限制窗口在屏幕范围内
            int screenWidth = GetSystemMetrics(SM_CXSCREEN);
            int screenHeight = GetSystemMetrics(SM_CYSCREEN);
            int windowWidth = window.getSize().x;
            int windowHeight = window.getSize().y;

            windowPosition.x = max(0, min(screenWidth - windowWidth, windowPosition.x));
            windowPosition.y = max(0, min(screenHeight - windowHeight, windowPosition.y));

            // 移动窗口
            HWND hwnd = window.getSystemHandle();
            SetWindowPos(hwnd, HWND_TOPMOST,
                         windowPosition.x, windowPosition.y,
                         0, 0, SWP_NOSIZE | SWP_NOACTIVATE);

            moveClock.restart();
            needsRedraw = true;
        }
    }

    // 关键修复：防止焦点被抢走
    void ensureFocusStaysOnGame() {
        if (focusCheckClock.getElapsedTime().asMilliseconds() < 500)
            return;

        HWND hwnd = window.getSystemHandle();
        HWND foreground = GetForegroundWindow();

        // 如果我们的窗口意外成为前台窗口
        if (foreground == hwnd) {
            // 立即放弃焦点
            SetFocus(NULL);

            // 尝试找到并激活游戏窗口
            if (gameWindowHandle && IsWindow(gameWindowHandle)) {
                // 恢复游戏窗口
                if (IsIconic(gameWindowHandle)) {
                    ShowWindow(gameWindowHandle, SW_RESTORE);
                }

                // 激活游戏窗口（但不要强制置顶，避免干扰）
                SetForegroundWindow(gameWindowHandle);
                Sleep(50);
            }

            // 重新设置我们的窗口样式
            makeWindowNonFocusable();

            cout << "Fixed unexpected focus gain" << endl;
        }

        focusCheckClock.restart();
    }

    void handleJoystickInput() {
        prevState = state; // 保存上一次状态
        DWORD dwResult = XInputGetState(0, &state);
        joystickConnected = (dwResult == ERROR_SUCCESS);

        if (!joystickConnected) return;

        // 如果窗口最小化了，只处理最小化/恢复功能
        if (isMinimized) {
            handleMinimizedState();
            return;
        }

        // 导航延迟控制
        if (inputClock.getElapsedTime().asMilliseconds() < 200) return;

        bool moved = false;
        float xAxis = state.Gamepad.sThumbLX;
        float yAxis = state.Gamepad.sThumbLY;

        // 左右移动（选择键盘按键）
        if (xAxis < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) {
            if (currentCol > 0) {
                currentCol--;
                moved = true;
            }
        } else if (xAxis > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) {
            if (currentCol < keys[currentRow].size() - 1) {
                currentCol++;
                moved = true;
            }
        }

        // 上下移动（选择键盘按键）
        if (yAxis > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) {
            if (currentRow > 0) {
                currentRow--;
                moved = true;
            }
        } else if (yAxis < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) {
            if (currentRow < keys.size() - 1) {
                currentRow++;
                moved = true;
            }
        }

        if (moved) {
            needsRedraw = true;
            selectionClock.restart(); // 重置选择高亮
            inputClock.restart();
        }

        // 处理十字键移动窗口
        moveWindowWithDPad();

        // === 按键映射 ===

        // 处理Y按钮（确认/选择键）- 关键修复：使用状态变化检测
        bool isYButtonPressed = (state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) != 0;
        bool wasYButtonPressed = (prevState.Gamepad.wButtons & XINPUT_GAMEPAD_Y) != 0;

        if (isYButtonPressed && !wasYButtonPressed) {
            // 在发送按键前，确保焦点在游戏窗口
            ensureFocusOnGameBeforeInput();

            selectKey();
            needsRedraw = true;
            buttonPressClock.restart();
        }

        // 处理A按钮（退出键）
        bool isAButtonPressed = (state.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0;
        bool wasAButtonPressed = (prevState.Gamepad.wButtons & XINPUT_GAMEPAD_A) != 0;

        if (isAButtonPressed && !wasAButtonPressed) {
            inputText.clear();
            needsRedraw = true;
        }

        // 处理B按钮（空格键）
        bool isBButtonPressed = (state.Gamepad.wButtons & XINPUT_GAMEPAD_B) != 0;
        bool wasBButtonPressed = (prevState.Gamepad.wButtons & XINPUT_GAMEPAD_B) != 0;

        if (isBButtonPressed && !wasBButtonPressed) {
            ensureFocusOnGameBeforeInput();
            queueKeyPress(VK_SPACE);
            inputText += " ";
            needsRedraw = true;
        }

        // 处理X按钮（退格键）
        bool isXButtonPressed = (state.Gamepad.wButtons & XINPUT_GAMEPAD_X) != 0;
        bool wasXButtonPressed = (prevState.Gamepad.wButtons & XINPUT_GAMEPAD_X) != 0;

        if (isXButtonPressed && !wasXButtonPressed) {
            ensureFocusOnGameBeforeInput();
            queueKeyPress(VK_BACK);
            if (!inputText.empty()) {
                inputText.pop_back();
            }
            needsRedraw = true;
        }

        // 处理LS按钮（左摇杆按下 - 切换大小写）
        bool isLSButtonPressed = (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
        bool wasLSButtonPressed = (prevState.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;

        if (isLSButtonPressed && !wasLSButtonPressed) {
            isUpperCase = !isUpperCase;
            updateKeyboardCase();
            needsRedraw = true;
        }

        // 处理Back+Start组合键（最小化/恢复窗口）
        bool isBackButtonPressed = (state.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) != 0;
        bool isStartButtonPressed = (state.Gamepad.wButtons & XINPUT_GAMEPAD_START) != 0;

        static bool backStartComboTriggered = false;
        if (isBackButtonPressed && isStartButtonPressed) {
            if (!backStartComboTriggered && comboKeyClock.getElapsedTime().asMilliseconds() > 500) {
                toggleWindowMinimize();
                backStartComboTriggered = true;
                comboKeyClock.restart();
            }
        } else {
            backStartComboTriggered = false;
        }

        // 处理RB+LB组合键退出程序
        bool isRBButtonPressed = (state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
        bool isLBButtonPressed = (state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;

        if (isRBButtonPressed && isLBButtonPressed) {
            if (!exitComboPressed) {
                exitComboPressed = true;
                exitClock.restart();
                cout << "Exit combo pressed, hold for 2 seconds to exit" << endl;
                needsRedraw = true;
            } else if (exitClock.getElapsedTime().asMilliseconds() > 2000) {
                cout << "Exit confirmed, closing window..." << endl;
                window.close();
                shouldExit = true;
                return;
            }
        } else {
            if (exitComboPressed) {
                exitComboPressed = false;
                needsRedraw = true;
            }
        }

        // 更新按钮状态
        buttonPressed = isYButtonPressed;
    }

    // 关键修复：在发送输入前确保焦点在游戏窗口
    void ensureFocusOnGameBeforeInput() {
        // 首先确保我们的窗口不是焦点
        HWND hwnd = window.getSystemHandle();
        HWND foreground = GetForegroundWindow();

        if (foreground == hwnd) {
            SetFocus(NULL);
            Sleep(10);
        }

        // 尝试激活游戏窗口
        if (gameWindowHandle && IsWindow(gameWindowHandle)) {
            // 确保游戏窗口没有最小化
            if (IsIconic(gameWindowHandle)) {
                ShowWindow(gameWindowHandle, SW_RESTORE);
                Sleep(50);
            }

            // 激活游戏窗口
            SetForegroundWindow(gameWindowHandle);
            Sleep(30); // 短暂延迟，让焦点切换生效

            // 重新设置我们的窗口为无焦点
            makeWindowNonFocusable();
        }
    }

    void handleMinimizedState() {
        // 窗口最小化时，只处理恢复功能
        bool isBackButtonPressed = (state.Gamepad.wButtons & XINPUT_GAMEPAD_BACK) != 0;
        bool isStartButtonPressed = (state.Gamepad.wButtons & XINPUT_GAMEPAD_START) != 0;

        static bool backStartComboTriggered = false;
        if (isBackButtonPressed && isStartButtonPressed) {
            if (!backStartComboTriggered && comboKeyClock.getElapsedTime().asMilliseconds() > 500) {
                restoreWindow();
                backStartComboTriggered = true;
                comboKeyClock.restart();
            }
        } else {
            backStartComboTriggered = false;
        }
    }

    void toggleWindowMinimize() {
        HWND hwnd = window.getSystemHandle();

        if (!isMinimized) {
            // 保存游戏窗口句柄
            if (!gameWindowHandle) {
                gameWindowHandle = GetForegroundWindow();
                if (gameWindowHandle == hwnd) {
                    gameWindowHandle = NULL;
                }
            }

            // 最小化窗口
            ShowWindow(hwnd, SW_MINIMIZE);
            isMinimized = true;
            wasMinimized = true;
            cout << "Window minimized" << endl;

            // 如果之前有游戏窗口，尝试恢复它
            if (gameWindowHandle && IsWindow(gameWindowHandle)) {
                SetForegroundWindow(gameWindowHandle);
            }
        } else {
            // 恢复窗口
            restoreWindow();
        }
    }

    void restoreWindow() {
        HWND hwnd = window.getSystemHandle();

        // 恢复窗口但不激活
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);

        // 重新应用无焦点窗口样式
        reapplyWindowStyle();

        isMinimized = false;

        // 如果之前最小化过，尝试恢复游戏窗口焦点
        if (wasMinimized && gameWindowHandle && IsWindow(gameWindowHandle)) {
            Sleep(100);
            SetForegroundWindow(gameWindowHandle);
            wasMinimized = false;
        }

        cout << "Window restored" << endl;
        needsRedraw = true;
    }

    // 将按键加入队列
    void queueKeyPress(WORD vkCode, bool needShift = false) {
        // 关键修复：发送前确保焦点在游戏窗口
        ensureFocusOnGameBeforeInput();

        // 短暂延迟，确保焦点切换完成
        Sleep(20);

        if (needShift) {
            std::lock_guard<std::mutex> lock(queueMutex);
            keyQueue.push(VK_SHIFT);
            keyQueue.push(vkCode);
            keyQueue.push(vkCode | 0x8000); // 释放按键
            keyQueue.push(VK_SHIFT | 0x8000);
        } else {
            std::lock_guard<std::mutex> lock(queueMutex);
            keyQueue.push(vkCode);
            keyQueue.push(vkCode | 0x8000); // 释放按键
        }
    }

    // 字符到虚拟键码的映射
    WORD charToVK(char c) {
        // 数字
        if (c >= '0' && c <= '9') {
            return c;
        }

        // 字母 - 转换为大写
        if (c >= 'a' && c <= 'z') {
            return toupper(c);
        }
        if (c >= 'A' && c <= 'Z') {
            return c;
        }

        // 特殊字符映射
        switch (c) {
            case ' ': return VK_SPACE;
            case ';': return VK_OEM_1;
            case ',': return VK_OEM_COMMA;
            case '.': return VK_OEM_PERIOD;
            case '/': return VK_OEM_2;
            case '`': return VK_OEM_3;
            case '[': return VK_OEM_4;
            case '\\': return VK_OEM_5;
            case ']': return VK_OEM_6;
            case '\'': return VK_OEM_7;
            case '-': return VK_OEM_MINUS;
            case '=': return VK_OEM_PLUS;
            default: return 0;
        }
    }

    // 判断字符是否需要Shift键
    bool needsShiftForChar(char c) {
        // 大写字母需要Shift
        if (c >= 'A' && c <= 'Z') {
            return true;
        }

        // 标点符号需要Shift
        switch (c) {
            case '!':
            case '@':
            case '#':
            case '$':
            case '%':
            case '^':
            case '&':
            case '*':
            case '(':
            case ')':
            case '_':
            case '+':
            case '{':
            case '}':
            case '|':
            case ':':
            case '"':
            case '<':
            case '>':
            case '?':
            case '~':
                return true;
            default:
                return false;
        }
    }

    void selectKey() {
        const string &key = keys[currentRow][currentCol].text;

        if (key == "SPACE") {
            queueKeyPress(VK_SPACE);
            inputText += " ";
        } else if (key == "BACK") {
            queueKeyPress(VK_BACK);
            if (!inputText.empty()) {
                inputText.pop_back();
            }
        } else if (key == "CLEAR") {
            // 清除文本
            for (size_t i = 0; i < inputText.length(); i++) {
                queueKeyPress(VK_BACK);
            }
            inputText.clear();
        } else if (key == "ENTER") {
            queueKeyPress(VK_RETURN);
            inputText.clear();
        } else if (key == "EXIT") {
            // 退出程序
            cout << "Exit button selected, closing window..." << endl;
            window.close();
            shouldExit = true;
            return;
        } else if (key.length() == 1) {
            // 普通字符
            char c = key[0];

            // 如果是字母，根据大小写状态调整
            if (isalpha(c)) {
                if (!isUpperCase) {
                    c = tolower(c);
                }
            }

            WORD vkCode = charToVK(c);
            if (vkCode != 0) {
                queueKeyPress(vkCode, needsShiftForChar(c));
                inputText += c;
            }
        }
        needsRedraw = true;
    }

public:
    VirtualKeyboard() : window(VideoMode(800, 400), "Virtual Keyboard", Style::None) {
        // 启动发送线程
        std::thread senderThread(keySenderThread);
        senderThread.detach();

        // 初始化字体和键盘
        initFont();
        initKeyboard();

        // 设置窗口属性
        window.setFramerateLimit(60);
        window.setVerticalSyncEnabled(true);

        // 延迟确保窗口完全创建
        Sleep(200);

        // 设置窗口为无焦点工具窗口
        makeWindowNonFocusable();

        // 记录游戏窗口
        gameWindowHandle = GetForegroundWindow();
        HWND hwnd = window.getSystemHandle();
        if (gameWindowHandle == hwnd) {
            gameWindowHandle = NULL;
        }

        cout << "Virtual Keyboard started" << endl;
        cout << "Window is set as non-focusable tool window" << endl;
        cout << "Game window handle: " << gameWindowHandle << endl;
        cout << "Controller mapping:" << endl;
        cout << "- Navigation: Left stick" << endl;
        cout << "- Select: Y button" << endl;
        cout << "- Exit/Clear: A button" << endl;
        cout << "- Space: B button" << endl;
        cout << "- Backspace: X button" << endl;
        cout << "- Toggle case: Press left stick (LS)" << endl;
        cout << "- Minimize/Restore: Back+Start combo (hold 0.5 seconds)" << endl;
        cout << "- Move window: DPad (arrow keys)" << endl;
        cout << "- Exit program: Hold RB+LB for 2 seconds OR select EXIT button" << endl;
    }

    ~VirtualKeyboard() {
        keepSending = false;
        Sleep(200);
    }

    void run() {
        Text inputDisplay("Input: ", font, 20);
        inputDisplay.setPosition(50, 20);
        inputDisplay.setFillColor(Color::Black);

        Text statusText("No Joystick", font, 16);
        statusText.setPosition(600, 20);
        statusText.setFillColor(Color::Black);

        Text caseText("CASE: UPPER", font, 14);
        caseText.setPosition(50, 70);
        caseText.setFillColor(Color::Blue);

        Text windowStateText("Window: Active", font, 14);
        windowStateText.setPosition(200, 70);
        windowStateText.setFillColor(Color::Green);

        Text moveHintText("Move: DPad", font, 14);
        moveHintText.setPosition(400, 70);
        moveHintText.setFillColor(Color(100, 100, 100));

        // 退出提示
        Text exitHintText("Exit: Hold RB+LB", font, 14);
        exitHintText.setPosition(550, 70);
        exitHintText.setFillColor(Color(150, 50, 50));

        // 帮助文本
        Text helpText(
            "Nav: Left Stick | Select: Y | Exit: A | Space: B | Backspace: X | Toggle Case: LS | Min/Max: Back+Start | Move: DPad | Exit Prog: RB+LB",
            font, 12);
        helpText.setPosition(50, 350);
        helpText.setFillColor(Color::Black);

        // 焦点状态显示
        Text focusText("Focus: OK", font, 12);
        focusText.setPosition(50, 380);
        focusText.setFillColor(Color::Green);

        while (window.isOpen()) {
            Event event;
            while (window.pollEvent(event)) {
                if (event.type == Event::Closed) {
                    window.close();
                    shouldExit = true;
                } else if (event.type == Event::GainedFocus) {
                    // 如果窗口意外获得焦点，立即重新设为无焦点
                    makeWindowNonFocusable();
                    needsRedraw = true;
                }
            }

            handleJoystickInput();

            // 定期检查并修复焦点
            ensureFocusStaysOnGame();

            // 更新焦点状态显示
            static Clock focusUpdateClock;
            if (focusUpdateClock.getElapsedTime().asMilliseconds() > 1000) {
                HWND foreground = GetForegroundWindow();
                HWND hwnd = window.getSystemHandle();

                if (foreground == hwnd) {
                    focusText.setString("Focus: LOST (fixing...)");
                    focusText.setFillColor(Color::Red);
                } else {
                    focusText.setString("Focus: OK");
                    focusText.setFillColor(Color::Green);
                }
                focusUpdateClock.restart();
            }

            // 控制重绘频率，解决闪烁问题
            if (needsRedraw || redrawClock.getElapsedTime().asMilliseconds() > 33) {
                // 大约30fps
                window.clear(Color(240, 240, 240));

                // 更新输入显示
                inputDisplay.setString("Input: " + inputText);
                window.draw(inputDisplay);

                // 更新大小写状态显示
                caseText.setString(isUpperCase ? "CASE: UPPER" : "CASE: lower");
                caseText.setFillColor(isUpperCase ? Color::Blue : Color::Red);
                window.draw(caseText);

                // 更新窗口状态显示
                windowStateText.setString(isMinimized ? "Window: Minimized" : "Window: Active");
                windowStateText.setFillColor(isMinimized ? Color::Red : Color::Green);
                window.draw(windowStateText);

                // 显示移动提示
                window.draw(moveHintText);

                // 显示退出提示
                window.draw(exitHintText);

                // 显示焦点状态
                window.draw(focusText);

                // 只有在窗口未最小化时才绘制键盘
                if (!isMinimized) {
                    // 绘制所有按键
                    for (int i = 0; i < keys.size(); i++) {
                        for (int j = 0; j < keys[i].size(); j++) {
                            // 设置当前选中按键的颜色 - 添加呼吸效果避免闪烁
                            if (i == currentRow && j == currentCol) {
                                // 呼吸效果：高亮亮度随时间变化
                                float time = selectionClock.getElapsedTime().asSeconds();
                                float pulse = (std::sin(time * 3.0f) + 1.0f) * 0.3f + 0.4f; // 0.4 到 1.0
                                Color highlightColor(100, 200, 255, static_cast<Uint8>(255 * pulse));
                                keys[i][j].shape.setFillColor(highlightColor);
                            } else {
                                keys[i][j].shape.setFillColor(keys[i][j].originalColor);
                            }

                            keys[i][j].draw(&window);
                        }
                    }
                } else {
                    // 窗口最小化时显示提示信息
                    Text minimizedText("Window Minimized\nPress Back+Start to restore", font, 24);
                    minimizedText.setFillColor(Color::Red);
                    minimizedText.setStyle(Text::Bold);

                    FloatRect textRect = minimizedText.getLocalBounds();
                    minimizedText.setOrigin(textRect.left + textRect.width / 2.0f,
                                            textRect.top + textRect.height / 2.0f);
                    minimizedText.setPosition(400, 200);

                    window.draw(minimizedText);
                }

                // 更新连接状态显示
                statusText.setString(joystickConnected ? "Joystick Connected" : "No Joystick");
                statusText.setFillColor(joystickConnected ? Color::Green : Color::Red);
                window.draw(statusText);

                // 显示退出组合键提示
                if (exitComboPressed) {
                    float progress = exitClock.getElapsedTime().asMilliseconds() / 2000.0f;
                    if (progress > 1.0f) progress = 1.0f;

                    RectangleShape progressBg(Vector2f(200, 20));
                    progressBg.setPosition(300, 320);
                    progressBg.setFillColor(Color(50, 50, 50));
                    progressBg.setOutlineColor(Color::Black);
                    progressBg.setOutlineThickness(2);

                    RectangleShape progressBar(Vector2f(200 * progress, 20));
                    progressBar.setPosition(300, 320);
                    progressBar.setFillColor(Color(255, 50, 50));

                    Text exitProgressText("Exiting...", font, 16);
                    exitProgressText.setPosition(300, 300);
                    exitProgressText.setFillColor(Color::Red);

                    window.draw(progressBg);
                    window.draw(progressBar);
                    window.draw(exitProgressText);
                }

                // 只有在窗口未最小化时才显示帮助文本
                if (!isMinimized) {
                    window.draw(helpText);
                }

                window.display();
                needsRedraw = false;
                redrawClock.restart();
            }

            sleep(milliseconds(10));
        }
    }
};

int main() {
    // 设置控制台输出为UTF-8
    SetConsoleOutputCP(65001);

    cout << "Starting Virtual Keyboard..." << endl;

    VirtualKeyboard keyboard;
    keyboard.run();

    if (shouldExit) {
        cout << "Virtual Keyboard exited successfully" << endl;
    }

    return 0;
}
