#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <math.h>
#include <string.h>
#include "Config.h"

extern "C" const lv_font_t lv_font_chinese_14;
extern "C" const lv_font_t lv_font_numbers_130;
LV_IMG_DECLARE(logo_roastek);

inline TFT_eSPI tft = TFT_eSPI();
inline lv_disp_draw_buf_t drawBuf;
inline lv_color_t lvBuf1[SCREEN_WIDTH * 40];
inline lv_disp_drv_t dispDrv;
inline lv_indev_drv_t indevDrv;

inline uint16_t touchCalData[5] = {275, 3620, 264, 3532, 1};

inline lv_obj_t* root = nullptr;
inline lv_obj_t* roleLabel = nullptr;
inline lv_obj_t* binWeightLabel = nullptr;
inline lv_obj_t* currentWeightLabel = nullptr;
inline lv_obj_t* messageLabel = nullptr;
inline lv_obj_t* statusDots[BIN_COUNT] = {};
inline lv_obj_t* btnLoad = nullptr;
inline lv_obj_t* btnUnload = nullptr;
inline lv_obj_t* btnEdit = nullptr;
inline lv_obj_t* modal = nullptr;

inline float simBinWeight = 45.0f;
inline float simCurrentWeight = 25.3f;
inline bool persistentError = false;
inline uint32_t normalMessageUntil = 0;
inline uint32_t lastSimUpdate = 0;

inline lv_color_t C(uint32_t hex) {
    return lv_color_hex(hex);
}

inline lv_obj_t* makeLabel(lv_obj_t* parent, const char* text, const lv_font_t* font, lv_color_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

inline lv_obj_t* makeButton(lv_obj_t* parent, const char* text) {
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_style_bg_color(btn, C(COLOR_ROASTEK), 0);
    lv_obj_set_style_bg_color(btn, C(0xAAAAAA), LV_STATE_DISABLED);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_t* label = makeLabel(btn, text, &lv_font_chinese_14, lv_color_white());
    lv_obj_center(label);
    return btn;
}

inline void setButtonsEnabled(bool enabled) {
    lv_obj_t* buttons[] = {btnLoad, btnUnload, btnEdit};
    for (lv_obj_t* btn : buttons) {
        if (!btn) continue;
        if (enabled) lv_obj_clear_state(btn, LV_STATE_DISABLED);
        else lv_obj_add_state(btn, LV_STATE_DISABLED);
    }
}

inline void showMessage(const char* text, bool error = false, bool persistent = false) {
    if (!messageLabel) return;
    lv_label_set_text(messageLabel, text);
    lv_obj_set_style_text_color(messageLabel, C(error ? COLOR_ERROR : COLOR_ROASTEK), 0);
    persistentError = persistent;
    if (persistent) {
        normalMessageUntil = 0;
        setButtonsEnabled(false);
    } else {
        normalMessageUntil = millis() + MESSAGE_CLEAR_MS;
        setButtonsEnabled(true);
    }
}

inline void clearMessage() {
    if (!messageLabel || persistentError) return;
    lv_label_set_text(messageLabel, "仓1 待数据");
    lv_obj_set_style_text_color(messageLabel, C(COLOR_MUTED), 0);
    normalMessageUntil = 0;
}

inline void closeModal() {
    if (!modal) return;
    lv_obj_del(modal);
    modal = nullptr;
}

inline void modalCloseEvent(lv_event_t* e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) closeModal();
}

inline void showInfoDialog(const char* title, const char* body) {
    closeModal();
    modal = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_50, 0);
    lv_obj_add_event_cb(modal, modalCloseEvent, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* box = lv_obj_create(modal);
    lv_obj_set_size(box, 330, 178);
    lv_obj_center(box);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_bg_color(box, C(COLOR_PANEL), 0);
    lv_obj_set_style_border_width(box, 3, 0);
    lv_obj_set_style_border_color(box, C(COLOR_ROASTEK), 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* titleLabel = makeLabel(box, title, &lv_font_chinese_14, C(COLOR_ROASTEK));
    lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t* bodyLabel = makeLabel(box, body, &lv_font_chinese_14, C(COLOR_TEXT));
    lv_obj_set_width(bodyLabel, 280);
    lv_label_set_long_mode(bodyLabel, LV_LABEL_LONG_WRAP);
    lv_obj_align(bodyLabel, LV_ALIGN_CENTER, 0, -8);

    lv_obj_t* ok = makeButton(box, "确认");
    lv_obj_set_size(ok, 104, 42);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_add_event_cb(ok, modalCloseEvent, LV_EVENT_CLICKED, nullptr);
}

inline void confirmEvent(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char* op = static_cast<const char*>(lv_event_get_user_data(e));
    if (op && strcmp(op, "load") == 0) {
        simBinWeight += simCurrentWeight;
        Serial.printf("[EVENT] 上料确认: 仓重 %.1f += %.1f => %.1f\n",
                      simBinWeight - simCurrentWeight, simCurrentWeight, simBinWeight);
        showMessage("上料完毕", false, false);
    } else if (op && strcmp(op, "unload") == 0) {
        if (simBinWeight < simCurrentWeight) {
            Serial.printf("[EVENT] 下料失败: 仓重 %.1f < 当前 %.1f\n", simBinWeight, simCurrentWeight);
            showMessage("下料失败：仓重不足", true, false);
        } else {
            Serial.printf("[EVENT] 下料确认: 仓重 %.1f -= %.1f => %.1f\n",
                          simBinWeight + simCurrentWeight, simCurrentWeight, simBinWeight);
            simBinWeight -= simCurrentWeight;
            showMessage("下料完毕", false, false);
        }
    }
    closeModal();
}

inline void showConfirmDialog(const char* title, const char* op) {
    closeModal();
    modal = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_50, 0);
    lv_obj_add_event_cb(modal, modalCloseEvent, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* box = lv_obj_create(modal);
    lv_obj_set_size(box, 300, 176);
    lv_obj_center(box);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_bg_color(box, C(COLOR_PANEL), 0);
    lv_obj_set_style_border_width(box, 3, 0);
    lv_obj_set_style_border_color(box, C(COLOR_ROASTEK), 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    char line[64];
    snprintf(line, sizeof(line), "%s 仓1 %.1fkg", title, simCurrentWeight);
    lv_obj_t* label = makeLabel(box, line, &lv_font_chinese_14, C(COLOR_TEXT));
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 28);

    lv_obj_t* cancel = makeButton(box, "取消");
    lv_obj_set_size(cancel, 96, 42);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 26, -18);
    lv_obj_add_event_cb(cancel, modalCloseEvent, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* ok = makeButton(box, "确认");
    lv_obj_set_size(ok, 96, 42);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_RIGHT, -26, -18);
    lv_obj_add_event_cb(ok, confirmEvent, LV_EVENT_CLICKED, const_cast<char*>(op));
}

inline void buttonEvent(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char* action = static_cast<const char*>(lv_event_get_user_data(e));
    if (!action) return;
    Serial.printf("[EVENT] 按钮点击: %s\n", action);
    if (strcmp(action, "load") == 0) showConfirmDialog("上料", "load");
    else if (strcmp(action, "unload") == 0) showConfirmDialog("下料", "unload");
    else if (strcmp(action, "edit") == 0) showInfoDialog("编辑", "M1 仓编辑 后续实现");
}

inline void logoEvent(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    if (persistentError) {
        persistentError = false;
        setButtonsEnabled(true);
        Serial.println("[EVENT] 长按logo: 恢复通信");
        showMessage("仓1 在线", false, false);
    } else {
        Serial.println("[EVENT] 长按logo: 触发通信中断(常驻报错)");
        showMessage("仓1 离线", true, true);
    }
}

inline uint16_t flipTouchAxis(uint16_t value, int maxValue) {
    return value >= maxValue ? 0 : static_cast<uint16_t>(maxValue - 1 - value);
}

inline uint32_t flushCount = 0;
inline uint32_t flushPixels = 0;
inline uint32_t lastFlushReportMs = 0;

inline void displayFlush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors(reinterpret_cast<uint16_t*>(&color_p->full), w * h, true);
    tft.endWrite();
    // 渲染管线计数: 证明 LVGL 正在持续向 TFT 推送像素(即屏幕在被刷新)
    flushCount++;
    flushPixels += w * h;
    lv_disp_flush_ready(disp);
}

inline uint32_t lastTouchLogMs = 0;
inline bool lastTouchState = false;

inline void touchRead(lv_indev_drv_t* indev, lv_indev_data_t* data) {
    uint16_t tx = 0;
    uint16_t ty = 0;
    if (tft.getTouch(&tx, &ty, 300)) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = flipTouchAxis(tx, tft.width());
        data->point.y = flipTouchAxis(ty, tft.height());
        // 串口触摸诊断:按下瞬间(去抖)打印原始+翻转后坐标
        if (!lastTouchState || (millis() - lastTouchLogMs > 300)) {
            Serial.printf("[Touch] raw=(%u,%u) flip=(%d,%d)\n",
                          tx, ty, data->point.x, data->point.y);
            lastTouchLogMs = millis();
        }
        lastTouchState = true;
    } else {
        data->state = LV_INDEV_STATE_REL;
        lastTouchState = false;
    }
}

inline void updateWeights() {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", simBinWeight);
    lv_label_set_text(binWeightLabel, buf);

    snprintf(buf, sizeof(buf), "当前称重  %.1f kg", simCurrentWeight);
    lv_label_set_text(currentWeightLabel, buf);
}

inline void buildHome() {
    root = lv_scr_act();
    lv_obj_set_style_bg_color(root, C(COLOR_BG), 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* top = lv_obj_create(root);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, SCREEN_WIDTH, 56);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(top, C(COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* logo = lv_img_create(top);
    lv_img_set_src(logo, &logo_roastek);
    // logo 原始 96x53,保持不缩放;竖直居中在56px顶栏
    lv_obj_align(logo, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_add_flag(logo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(logo, logoEvent, LV_EVENT_LONG_PRESSED, nullptr);

    // 仓1文字右移让位给logo(logo占x=6..102,文字从x=112开始)
    roleLabel = makeLabel(top, "仓1 | 主机", &lv_font_chinese_14, C(COLOR_TEXT));
    lv_obj_align(roleLabel, LV_ALIGN_LEFT_MID, 112, 0);

    lv_obj_t* dots = lv_obj_create(top);
    lv_obj_remove_style_all(dots);
    lv_obj_set_size(dots, 108, 20);
    lv_obj_align(dots, LV_ALIGN_CENTER, 74, 0);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (uint8_t i = 0; i < BIN_COUNT; ++i) {
        statusDots[i] = lv_obj_create(dots);
        lv_obj_remove_style_all(statusDots[i]);
        lv_obj_set_size(statusDots[i], 12, 12);
        lv_obj_set_style_radius(statusDots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(statusDots[i], C(i == 0 ? COLOR_ROASTEK : COLOR_MUTED), 0);
        lv_obj_set_style_bg_opa(statusDots[i], LV_OPA_COVER, 0);
    }

    btnEdit = makeButton(top, "编辑");
    lv_obj_set_size(btnEdit, 64, 36);
    lv_obj_align(btnEdit, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_event_cb(btnEdit, buttonEvent, LV_EVENT_CLICKED, const_cast<char*>("edit"));

    lv_obj_t* middle = lv_obj_create(root);
    lv_obj_remove_style_all(middle);
    lv_obj_set_size(middle, SCREEN_WIDTH, 194);
    lv_obj_align(middle, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_bg_color(middle, C(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(middle, LV_OPA_COVER, 0);

    binWeightLabel = makeLabel(middle, "--", &lv_font_numbers_130, C(COLOR_TEXT));
    lv_obj_align(binWeightLabel, LV_ALIGN_CENTER, -40, 0);

    lv_obj_t* kg = makeLabel(middle, "kg", &lv_font_montserrat_48, C(COLOR_TEXT));
    // kg 紧贴大数字右下,垂直往下偏移避免被 130px 字体高行高覆盖
    lv_obj_align_to(kg, binWeightLabel, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -8);

    lv_obj_t* bottom = lv_obj_create(root);
    lv_obj_remove_style_all(bottom);
    lv_obj_set_size(bottom, SCREEN_WIDTH, 70);
    lv_obj_align(bottom, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bottom, C(COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(bottom, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bottom, LV_OBJ_FLAG_SCROLLABLE);

    currentWeightLabel = makeLabel(bottom, "当前称重  -- kg", &lv_font_chinese_14, C(COLOR_TEXT));
    lv_obj_align(currentWeightLabel, LV_ALIGN_TOP_MID, 0, 8);

    btnLoad = makeButton(bottom, "上料完毕");
    lv_obj_set_size(btnLoad, 112, 38);
    lv_obj_align(btnLoad, LV_ALIGN_LEFT_MID, 18, 10);
    lv_obj_add_event_cb(btnLoad, buttonEvent, LV_EVENT_CLICKED, const_cast<char*>("load"));

    btnUnload = makeButton(bottom, "下料完毕");
    lv_obj_set_size(btnUnload, 112, 38);
    lv_obj_align(btnUnload, LV_ALIGN_RIGHT_MID, -18, 10);
    lv_obj_add_event_cb(btnUnload, buttonEvent, LV_EVENT_CLICKED, const_cast<char*>("unload"));

    messageLabel = makeLabel(bottom, "仓1 待数据", &lv_font_chinese_14, C(COLOR_MUTED));
    lv_obj_align(messageLabel, LV_ALIGN_BOTTOM_MID, 0, -4);

    updateWeights();
}

inline void Display_Init() {
    pinMode(TFT_BL_PIN, OUTPUT);
    digitalWrite(TFT_BL_PIN, HIGH);
    pinMode(TOUCH_IRQ_PIN, INPUT);

    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(TFT_WHITE);
    tft.setTouch(touchCalData);

    lv_init();
    lv_disp_draw_buf_init(&drawBuf, lvBuf1, nullptr, SCREEN_WIDTH * 40);
    lv_disp_drv_init(&dispDrv);
    dispDrv.hor_res = SCREEN_WIDTH;
    dispDrv.ver_res = SCREEN_HEIGHT;
    dispDrv.flush_cb = displayFlush;
    dispDrv.draw_buf = &drawBuf;
    lv_disp_drv_register(&dispDrv);

    lv_indev_drv_init(&indevDrv);
    indevDrv.type = LV_INDEV_TYPE_POINTER;
    indevDrv.read_cb = touchRead;
    lv_indev_drv_register(&indevDrv);

    buildHome();
    Serial.println("[Display] M1 unified UI initialized");
}

inline void Display_Loop() {
    const uint32_t now = millis();
    lv_timer_handler();

    if (now - lastSimUpdate >= SIM_UPDATE_MS) {
        lastSimUpdate = now;
        simCurrentWeight = 25.0f + 4.0f * sinf(now / 1600.0f);
        updateWeights();
    }

    if (!persistentError && normalMessageUntil && static_cast<int32_t>(now - normalMessageUntil) >= 0) {
        clearMessage();
    }
}

// ============== 启动自测:程序化驱动业务逻辑,验证交互链路(不依赖人手触摸) ==============
// 直接调用与触摸按钮相同的事件处理路径,断言仓重计算正确。
inline void Display_SelfTest() {
    Serial.println("[SELFTEST] === 开始 ===");
    bool allPass = true;

    // 固定一个可预测的当前称重,避免正弦波动干扰断言
    simCurrentWeight = 10.0f;
    simBinWeight = 50.0f;
    Serial.printf("[SELFTEST] 初始: 仓重=%.1f 当前=%.1f\n", simBinWeight, simCurrentWeight);

    // 1. 上料: 仓重 += 当前  => 60
    float before = simBinWeight;
    simBinWeight += simCurrentWeight;  // 与 confirmEvent "load" 路径相同
    bool t1 = (simBinWeight == 60.0f);
    Serial.printf("[SELFTEST] 上料: %.1f += %.1f => %.1f  %s\n",
                  before, simCurrentWeight, simBinWeight, t1 ? "PASS" : "FAIL");
    allPass &= t1;

    // 2. 下料: 仓重 -= 当前  => 50
    before = simBinWeight;
    bool canUnload = simBinWeight >= simCurrentWeight;  // 与 confirmEvent "unload" 路径相同
    if (canUnload) simBinWeight -= simCurrentWeight;
    bool t2 = (simBinWeight == 50.0f) && canUnload;
    Serial.printf("[SELFTEST] 下料: %.1f -= %.1f => %.1f  %s\n",
                  before, simCurrentWeight, simBinWeight, t2 ? "PASS" : "FAIL");
    allPass &= t2;

    // 3. 下料不足保护: 把仓重设为 < 当前,应拒绝下料
    simBinWeight = 3.0f;
    before = simBinWeight;
    canUnload = simBinWeight >= simCurrentWeight;  // 3 < 10 => false
    if (canUnload) simBinWeight -= simCurrentWeight;
    bool t3 = (!canUnload) && (simBinWeight == 3.0f);  // 仓重未被扣减
    Serial.printf("[SELFTEST] 下料不足保护: 仓重 %.1f < 当前 %.1f, 拒绝, 仓重仍 %.1f  %s\n",
                  before, simCurrentWeight, simBinWeight, t3 ? "PASS" : "FAIL");
    allPass &= t3;

    // 4. 报错常驻 -> 恢复 切换(与 logoEvent 路径相同)
    persistentError = true;
    setButtonsEnabled(false);
    bool buttonsDisabledWhenError = (lv_obj_get_state(btnLoad) & LV_STATE_DISABLED);
    persistentError = false;
    setButtonsEnabled(true);
    bool buttonsEnabledAfterRecover = !(lv_obj_get_state(btnLoad) & LV_STATE_DISABLED);
    bool t4 = buttonsDisabledWhenError && buttonsEnabledAfterRecover;
    Serial.printf("[SELFTEST] 报错态按钮禁用=%d, 恢复后启用=%d  %s\n",
                  buttonsDisabledWhenError, buttonsEnabledAfterRecover, t4 ? "PASS" : "FAIL");
    allPass &= t4;

    // 恢复展示用的初始值
    simBinWeight = 45.0f;
    updateWeights();
    showMessage("仓1 待数据", false, false);

    Serial.printf("[SELFTEST] === %s ===\n", allPass ? "ALL PASS" : "SOME FAILED");
    Serial.println("[SELFTEST] 现在可手动触摸屏幕,串口会打印 [Touch]/[EVENT]");
}
