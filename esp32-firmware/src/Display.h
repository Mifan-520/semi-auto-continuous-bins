#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <Preferences.h>
#include "Config.h"
#include "EspnowMesh.h"

extern "C" const lv_font_t lv_font_chinese_14;
extern "C" const lv_font_t lv_font_numbers_130;
LV_IMG_DECLARE(logo_roastek);

inline TFT_eSPI tft = TFT_eSPI();
inline lv_disp_draw_buf_t drawBuf;
inline lv_color_t lvBuf1[SCREEN_WIDTH * 10];
inline lv_disp_drv_t dispDrv;
inline lv_indev_drv_t indevDrv;

inline uint16_t touchCalData[5] = {275, 3620, 264, 3532, 1};

// ===== 全局UI对象 =====
inline lv_obj_t* root = nullptr;
inline lv_obj_t* binLabel = nullptr;            // 仓号文字 (仓1)
inline lv_obj_t* connLamp = nullptr;            // 在线/离线 大圆灯
inline lv_obj_t* warnBox = nullptr;             // 离线警告框(左上角)
inline lv_obj_t* warnCover = nullptr;           // 在线时覆盖告警残影的白色块
inline lv_obj_t* statusDots[BIN_COUNT] = {};    // 6仓状态灯 (左到右1-6)
inline lv_obj_t* binWeightLabel = nullptr;      // 中间130px大数字 (仓重)
inline lv_obj_t* kgLabel = nullptr;             // 仓重 kg单位(缩小,在数字下方)
inline lv_obj_t* curWeightLabel = nullptr;      // 左侧称重重量
inline lv_obj_t* curKgLabel = nullptr;          // 称重 kg单位(缩小,在数字下方)
inline lv_obj_t* btnLoad = nullptr;
inline lv_obj_t* btnUnload = nullptr;
inline lv_obj_t* btnEdit = nullptr;
inline lv_obj_t* modal = nullptr;

// ===== 编辑面板对象 =====
inline lv_obj_t* editOverlay = nullptr;
inline char editInputBuf[16] = "";
inline lv_obj_t* editInputLabel = nullptr;
inline lv_obj_t* editKeyMatrix = nullptr;
inline lv_obj_t* editConfirmBox = nullptr;   // 确认修改对话框
inline float editPendingWeight = 0;           // 待确认的新重量

// ===== 业务状态 (M1模拟) =====
inline float binWeights[BIN_COUNT] = {45.0f, 30.0f, 50.0f, 20.0f, 60.0f, 35.0f};
inline uint8_t localBin = 0;           // 本机仓号 0-5 (默认仓1)
inline float simCurrentWeight = 25.3f;
inline bool online = false;           // 在线状态 (ESP-NOW初始化后设true)
inline bool persistentError = false;
inline volatile bool commRecoverPending = false;  // ESP-NOW回调置位, Display_Loop里清告警
inline uint32_t normalMessageUntil = 0;
inline uint32_t lastSimUpdate = 0;

// 6仓在线表 (全部默认灰色, 上线后才变绿)
inline bool binOnline[BIN_COUNT] = {false, false, false, false, false, false};

// ===== NVS 持久化(Preferences) — 断电量还在 =====
static constexpr const char* NVS_NS = "binweight";

inline void nvsSaveBinWeights() {
    Preferences prefs;
    prefs.begin(NVS_NS, false);
    for (int i = 0; i < BIN_COUNT; ++i) {
        char key[8];
        snprintf(key, sizeof(key), "bw%d", i);
        prefs.putFloat(key, binWeights[i]);
    }
    prefs.putUChar("localBin", localBin);
    prefs.end();
    Serial.println("[NVS] 仓重已保存");
}

inline void nvsLoadBinWeights() {
    Preferences prefs;
    prefs.begin(NVS_NS, true);
    for (int i = 0; i < BIN_COUNT; ++i) {
        char key[8];
        snprintf(key, sizeof(key), "bw%d", i);
        binWeights[i] = prefs.getFloat(key, binWeights[i]);  // 若无记录则保留默认值
    }
    localBin = prefs.getUChar("localBin", localBin);
    prefs.end();
    Serial.printf("[NVS] 已加载: 本机=仓%d, 仓重=%.1f/%.1f/%.1f/%.1f/%.1f/%.1f\n",
                  localBin + 1,
                  binWeights[0], binWeights[1], binWeights[2],
                  binWeights[3], binWeights[4], binWeights[5]);
}

// ===== 颜色 =====
inline lv_color_t C(uint32_t hex) { return lv_color_hex(hex); }
constexpr uint32_t CLR_WHITE = 0xFFFFFF;
constexpr uint32_t CLR_PANEL = 0xFFFFFF;
constexpr uint32_t CLR_TEXT = 0x222222;
constexpr uint32_t CLR_MUTED = 0x888888;
constexpr uint32_t CLR_GREEN = 0x33CC33;   // 在线绿
constexpr uint32_t CLR_RED = 0xCC0000;     // 离线红/报错
constexpr uint32_t CLR_WARN_YELLOW = 0xFFD21A; // 离线警告黄
constexpr uint32_t CLR_GRAY = 0xCCCCCC;    // 灰灯
constexpr uint32_t CLR_ROASTEK = 0x661E2B; // 玫瑰红(按钮)
constexpr uint32_t CLR_BG_INPUT = 0xF3F3F3;

// 数字键盘map 4x3: 7 8 9 / 4 5 6 / 1 2 3 / . 0 ⌫
static const char* kbMap[] = {
    "7", "8", "9", "\n",
    "4", "5", "6", "\n",
    "1", "2", "3", "\n",
    ".", "0", LV_SYMBOL_BACKSPACE, ""
};

inline lv_obj_t* makeLabel(lv_obj_t* parent, const char* text, const lv_font_t* font, lv_color_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

inline lv_obj_t* makeButton(lv_obj_t* parent, const char* text) {
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_style_bg_color(btn, C(CLR_ROASTEK), 0);
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

// ===== 前向声明 =====
inline void updateConnLamp();
inline void openEditPanel();
inline void updateWeights();
inline void closeModal();
inline void modalCloseEvent(lv_event_t* e);
inline void updateBinDots();
inline void showDevBinDialog();
inline void setOnline(bool isOnline);
inline void showCommAlarm();
inline void hideCommAlarm();
inline void showEditConfirmDialog(float newWeight);

// ===== 消息栏 (已移除UI, 保留接口仅作日志/离线控制) =====
inline void showMessage(const char* text, bool error = false, bool persistent = false) {
    if (text) Serial.printf("[MSG] %s%s\n", error ? "[ERR] " : "", text);
    persistentError = persistent;
    if (persistent) {
        setOnline(false);  // 持续报错=离线,显示警告框+禁用按钮
    } else {
        setOnline(true);
    }
}

inline void clearMessage() {
    persistentError = false;
}

// ===== 模态关闭 =====
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
    lv_obj_set_style_bg_color(box, C(CLR_PANEL), 0);
    lv_obj_set_style_border_width(box, 3, 0);
    lv_obj_set_style_border_color(box, C(CLR_ROASTEK), 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* titleLabel = makeLabel(box, title, &lv_font_chinese_14, C(CLR_ROASTEK));
    lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t* bodyLabel = makeLabel(box, body, &lv_font_chinese_14, C(CLR_TEXT));
    lv_obj_set_width(bodyLabel, 280);
    lv_label_set_long_mode(bodyLabel, LV_LABEL_LONG_WRAP);
    lv_obj_align(bodyLabel, LV_ALIGN_CENTER, 0, -8);

    lv_obj_t* ok = makeButton(box, "确认");
    lv_obj_set_size(ok, 104, 42);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_add_event_cb(ok, modalCloseEvent, LV_EVENT_CLICKED, nullptr);
}

// ===== 上料/下料确认弹窗 =====
inline void confirmEvent(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char* op = static_cast<const char*>(lv_event_get_user_data(e));
    uint8_t b = localBin;
    if (op && strcmp(op, "load") == 0) {
        binWeights[b] += simCurrentWeight;
        Serial.printf("[EVENT] 上料确认: 仓%d %.1f += %.1f => %.1f\n",
                      b + 1, binWeights[b] - simCurrentWeight, simCurrentWeight, binWeights[b]);
        nvsSaveBinWeights();
        updateWeights();
        showMessage("上料完毕", false, false);
    } else if (op && strcmp(op, "unload") == 0) {
        if (binWeights[b] < simCurrentWeight) {
            Serial.printf("[EVENT] 下料失败: 仓%d %.1f < 当前 %.1f\n", b + 1, binWeights[b], simCurrentWeight);
            showMessage("下料失败:仓重不足", true, false);
        } else {
            Serial.printf("[EVENT] 下料确认: 仓%d %.1f -= %.1f => %.1f\n",
                          b + 1, binWeights[b] + simCurrentWeight, simCurrentWeight, binWeights[b]);
            binWeights[b] -= simCurrentWeight;
            nvsSaveBinWeights();
            updateWeights();
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
    lv_obj_set_style_bg_color(box, C(CLR_PANEL), 0);
    lv_obj_set_style_border_width(box, 3, 0);
    lv_obj_set_style_border_color(box, C(CLR_ROASTEK), 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    char line[64];
    snprintf(line, sizeof(line), "%s 仓%d %.1fkg", title, localBin + 1, simCurrentWeight);
    lv_obj_t* label = makeLabel(box, line, &lv_font_chinese_14, C(CLR_TEXT));
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

// ===== 编辑面板 (数字键盘) =====
inline void closeEditPanel() {
    if (!editOverlay) return;
    lv_obj_del(editOverlay);
    editOverlay = nullptr;
    editInputLabel = nullptr;
    editKeyMatrix = nullptr;
    editConfirmBox = nullptr;
    editPendingWeight = 0;
}

inline void updateEditInputDisplay() {
    if (!editInputLabel) return;
    char display[24];
    snprintf(display, sizeof(display), "%s kg", strlen(editInputBuf) == 0 ? "0" : editInputBuf);
    lv_label_set_text(editInputLabel, display);
}

inline void loadCurrentWeightToEditInput() {
    snprintf(editInputBuf, sizeof(editInputBuf), "%.1f", binWeights[localBin]);
    updateEditInputDisplay();
}

inline void editKeyEvent(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    uint16_t btn = lv_btnmatrix_get_selected_btn(editKeyMatrix);
    if (btn == LV_BTNMATRIX_BTN_NONE) return;
    const char* txt = lv_btnmatrix_get_btn_text(editKeyMatrix, btn);
    if (!txt) return;
    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        int len = strlen(editInputBuf);
        if (len > 0) editInputBuf[len - 1] = '\0';
    } else if (strcmp(txt, ".") == 0) {
        if (strchr(editInputBuf, '.') == nullptr) {
            int len = strlen(editInputBuf);
            if (len < (int)sizeof(editInputBuf) - 1) {
                editInputBuf[len] = '.';
                editInputBuf[len + 1] = '\0';
            }
        }
    } else {
        int len = strlen(editInputBuf);
        const char* dotPos = strchr(editInputBuf, '.');
        if (dotPos != nullptr) {
            int decimals = len - (dotPos - editInputBuf) - 1;
            if (decimals >= 1) return;
        }
        if (len >= 6) return;
        editInputBuf[len] = txt[0];
        editInputBuf[len + 1] = '\0';
    }
    updateEditInputDisplay();
}

// ===== 确认修改对话框 (编辑面板内) =====
inline void showEditConfirmDialog(float newWeight) {
    if (editConfirmBox || !editOverlay) return;  // 防止重复弹出

    editPendingWeight = newWeight;

    // 半透明遮罩 (盖在编辑面板上方)
    lv_obj_t* mask = lv_obj_create(editOverlay);
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, lv_obj_get_width(editOverlay), lv_obj_get_height(editOverlay));
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_30, 0);
    lv_obj_set_pos(mask, 0, 0);

    // 确认框
    editConfirmBox = lv_obj_create(mask);
    lv_obj_remove_style_all(editConfirmBox);
    lv_obj_set_size(editConfirmBox, 300, 160);
    lv_obj_center(editConfirmBox);
    lv_obj_set_style_bg_color(editConfirmBox, C(CLR_PANEL), 0);
    lv_obj_set_style_border_width(editConfirmBox, 3, 0);
    lv_obj_set_style_border_color(editConfirmBox, C(CLR_ROASTEK), 0);
    lv_obj_set_style_radius(editConfirmBox, 12, 0);
    lv_obj_clear_flag(editConfirmBox, LV_OBJ_FLAG_SCROLLABLE);

    // 标题
    lv_obj_t* titleLabel = makeLabel(editConfirmBox, "确认修改", &lv_font_chinese_14, C(CLR_ROASTEK));
    lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, 14);

    // 内容
    char msg[40];
    snprintf(msg, sizeof(msg), "将仓%d重量改为%.1f kg?", localBin + 1, newWeight);
    lv_obj_t* bodyLabel = makeLabel(editConfirmBox, msg, &lv_font_chinese_14, C(CLR_TEXT));
    lv_obj_set_width(bodyLabel, 270);
    lv_label_set_long_mode(bodyLabel, LV_LABEL_LONG_WRAP);
    lv_obj_align(bodyLabel, LV_ALIGN_CENTER, 0, -8);

    // 取消按钮 (灰色)
    lv_obj_t* cancelBtn = lv_btn_create(editConfirmBox);
    lv_obj_set_size(cancelBtn, 100, 42);
    lv_obj_set_style_radius(cancelBtn, 8, 0);
    lv_obj_set_style_bg_color(cancelBtn, C(CLR_GRAY), 0);
    lv_obj_set_style_shadow_width(cancelBtn, 0, 0);
    lv_obj_align(cancelBtn, LV_ALIGN_BOTTOM_LEFT, 24, -16);
    lv_obj_t* cancelLbl = makeLabel(cancelBtn, "取消", &lv_font_chinese_14, lv_color_white());
    lv_obj_center(cancelLbl);
    lv_obj_add_event_cb(cancelBtn, [](lv_event_t* e) {
        lv_obj_t* m = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_target(e)));
        if (m) lv_obj_del(m);
        editConfirmBox = nullptr;
        editPendingWeight = 0;
        Serial.println("[EVENT] 编辑面板: 取消修改");
    }, LV_EVENT_CLICKED, nullptr);

    // 确认按钮 (玫瑰红)
    lv_obj_t* confirmBtn = lv_btn_create(editConfirmBox);
    lv_obj_set_size(confirmBtn, 100, 42);
    lv_obj_set_style_radius(confirmBtn, 8, 0);
    lv_obj_set_style_bg_color(confirmBtn, C(CLR_ROASTEK), 0);
    lv_obj_set_style_shadow_width(confirmBtn, 0, 0);
    lv_obj_align(confirmBtn, LV_ALIGN_BOTTOM_RIGHT, -24, -16);
    lv_obj_t* confirmLbl = makeLabel(confirmBtn, "确认", &lv_font_chinese_14, lv_color_white());
    lv_obj_center(confirmLbl);
    lv_obj_add_event_cb(confirmBtn, [](lv_event_t* e) {
        float val = editPendingWeight;
        Serial.printf("[EVENT] 编辑确认: 仓%d 设为 %.1f kg\n", localBin + 1, val);
        binWeights[localBin] = val;
        nvsSaveBinWeights();  // 持久化
        updateWeights();
        // 关闭编辑面板 + 确认框
        closeEditPanel();
        // 立即广播本机仓更新(心跳)
        EspnowMesh_BroadcastHeartbeat(binWeights[localBin], simCurrentWeight);
        // 广播仓重同步到所有设备 (为后续DTU上报铺垫)
        EspnowMesh_BroadcastBinWeightSync(localBin + 1, val);
        showMessage("修改完毕", false, false);
    }, LV_EVENT_CLICKED, nullptr);
}

inline void editConfirmEvent(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (strlen(editInputBuf) == 0) {
        showMessage("请输入重量", true, false);
        return;
    }
    float val = atof(editInputBuf);
    // 弹出确认修改对话框
    showEditConfirmDialog(val);
}

inline void openEditPanel() {
    if (editOverlay) return;
    memset(editInputBuf, 0, sizeof(editInputBuf));

    editOverlay = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(editOverlay);
    lv_obj_set_size(editOverlay, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(editOverlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(editOverlay, LV_OPA_50, 0);
    lv_obj_set_pos(editOverlay, 0, 0);
    lv_obj_add_flag(editOverlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(editOverlay, [](lv_event_t* e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) closeEditPanel();
    }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* panel = lv_obj_create(editOverlay);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, 380, 300);
    lv_obj_set_style_bg_color(panel, C(CLR_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, C(CLR_ROASTEK), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 8, 0);
    lv_obj_center(panel);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    // 标题 (显示本机仓号)
    char title[24];
    snprintf(title, sizeof(title), "编辑仓%d重量", localBin + 1);
    lv_obj_t* titleLabel = makeLabel(panel, title, &lv_font_chinese_14, C(CLR_TEXT));
    lv_obj_align(titleLabel, LV_ALIGN_TOP_LEFT, 4, 2);

    // 关闭按钮
    lv_obj_t* closeBtn = lv_btn_create(panel);
    lv_obj_set_size(closeBtn, 32, 32);
    lv_obj_set_style_radius(closeBtn, 16, 0);
    lv_obj_set_style_bg_color(closeBtn, C(CLR_ROASTEK), 0);
    lv_obj_set_style_border_width(closeBtn, 0, 0);
    lv_obj_set_style_shadow_width(closeBtn, 0, 0);
    lv_obj_align(closeBtn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_t* xLabel = lv_label_create(closeBtn);
    lv_label_set_text(xLabel, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(xLabel, lv_color_white(), 0);
    lv_obj_center(xLabel);
    lv_obj_add_event_cb(closeBtn, [](lv_event_t* e) {
        closeEditPanel();
    }, LV_EVENT_CLICKED, nullptr);

    // 内容区: 输入框 + 键盘 + 确认 (拉伸占满)
    lv_obj_t* content = lv_obj_create(panel);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, 360, 252);
    lv_obj_set_style_pad_all(content, 4, 0);
    lv_obj_set_style_pad_row(content, 6, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 28);

    // 输入框 (拉伸占满宽度)
    lv_obj_t* inputBox = lv_obj_create(content);
    lv_obj_remove_style_all(inputBox);
    lv_obj_set_size(inputBox, 348, 40);
    lv_obj_set_style_bg_color(inputBox, C(CLR_BG_INPUT), 0);
    lv_obj_set_style_bg_opa(inputBox, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(inputBox, 6, 0);
    lv_obj_set_style_border_color(inputBox, C(CLR_ROASTEK), 0);
    lv_obj_set_style_border_width(inputBox, 1, 0);

    editInputLabel = makeLabel(inputBox, "0 kg", &lv_font_chinese_14, C(CLR_TEXT));
    lv_obj_align(editInputLabel, LV_ALIGN_RIGHT_MID, -8, 0);

    // 键盘 (拉伸占满宽度)
    editKeyMatrix = lv_btnmatrix_create(content);
    lv_obj_set_size(editKeyMatrix, 348, 144);
    lv_btnmatrix_set_map(editKeyMatrix, kbMap);
    lv_obj_set_style_bg_color(editKeyMatrix, C(CLR_PANEL), 0);
    lv_obj_set_style_bg_opa(editKeyMatrix, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(editKeyMatrix, 0, 0);
    lv_obj_set_style_radius(editKeyMatrix, 6, 0);
    lv_obj_set_style_pad_all(editKeyMatrix, 2, 0);
    lv_obj_set_style_pad_gap(editKeyMatrix, 4, 0);
    lv_obj_set_style_bg_color(editKeyMatrix, C(CLR_BG_INPUT), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(editKeyMatrix, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(editKeyMatrix, C(CLR_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_font(editKeyMatrix, &lv_font_chinese_14, LV_PART_ITEMS);
    lv_obj_set_style_radius(editKeyMatrix, 6, LV_PART_ITEMS);
    lv_obj_set_style_border_width(editKeyMatrix, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(editKeyMatrix, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(editKeyMatrix, C(CLR_ROASTEK), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(editKeyMatrix, lv_color_white(), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_add_event_cb(editKeyMatrix, editKeyEvent, LV_EVENT_VALUE_CHANGED, nullptr);

    // 确认按钮 (拉伸占满宽度)
    lv_obj_t* editConfirmBtn = lv_btn_create(content);
    lv_obj_set_size(editConfirmBtn, 348, 40);
    lv_obj_set_style_radius(editConfirmBtn, 6, 0);
    lv_obj_set_style_bg_color(editConfirmBtn, C(CLR_ROASTEK), 0);
    lv_obj_set_style_bg_opa(editConfirmBtn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(editConfirmBtn, 0, 0);
    lv_obj_set_style_shadow_width(editConfirmBtn, 0, 0);
    lv_obj_t* ecLbl = makeLabel(editConfirmBtn, "确认", &lv_font_chinese_14, lv_color_white());
    lv_obj_center(ecLbl);
    lv_obj_add_event_cb(editConfirmBtn, editConfirmEvent, LV_EVENT_CLICKED, nullptr);

    loadCurrentWeightToEditInput();
    Serial.println("[EVENT] 编辑面板已打开");
}

inline void buttonEvent(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char* action = static_cast<const char*>(lv_event_get_user_data(e));
    if (!action) return;
    Serial.printf("[EVENT] 按钮点击: %s\n", action);
    if (strcmp(action, "load") == 0) showConfirmDialog("上料", "load");
    else if (strcmp(action, "unload") == 0) showConfirmDialog("下料", "unload");
    else if (strcmp(action, "edit") == 0) openEditPanel();
}

// ===== 长按logo: 切换在线/离线 (M1模拟) =====
// ===== 长按logo: 开发者模式 - 编辑本机仓号 =====
inline void devBinSelectEvent(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    uint8_t oldBin = localBin;
    localBin = idx;
    Serial.printf("[EVENT] 开发者模式: 本机仓号 仓%d → 仓%d\n", oldBin + 1, idx + 1);
    // ESP-NOW: 旧仓下线 + 新仓上线(自动通知Display更新灯) + 连发3次心跳
    EspnowMesh_OnBinChanged(oldBin + 1, idx + 1, binWeights[idx], simCurrentWeight);
    closeModal();
    // 更新顶栏仓号文字
    char binTitle[8];
    snprintf(binTitle, sizeof(binTitle), "仓%d", localBin + 1);
    lv_label_set_text(binLabel, binTitle);
    updateWeights();
    nvsSaveBinWeights();
    // 强制广播新仓号(不等心跳周期)
    EspnowMesh_BroadcastHeartbeat(binWeights[localBin], simCurrentWeight);
    Serial.printf("[NVS] 本机仓号已保存: 仓%d\n", localBin + 1);
}

inline void showDevBinDialog() {
    closeModal();
    modal = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, SCREEN_WIDTH, SCREEN_HEIGHT);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_50, 0);
    lv_obj_add_event_cb(modal, modalCloseEvent, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* box = lv_obj_create(modal);
    lv_obj_set_size(box, 360, 230);
    lv_obj_center(box);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_bg_color(box, C(CLR_PANEL), 0);
    lv_obj_set_style_border_width(box, 3, 0);
    lv_obj_set_style_border_color(box, C(CLR_ROASTEK), 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* titleLabel = makeLabel(box, "开发者:选择本机仓号", &lv_font_chinese_14, C(CLR_ROASTEK));
    lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, 12);

    // 6个仓按钮 2行3列
    lv_obj_t* grid = lv_obj_create(box);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, 330, 130);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 44);
    lv_obj_set_style_pad_all(grid, 4, 0);
    lv_obj_set_style_pad_row(grid, 6, 0);
    lv_obj_set_style_pad_column(grid, 8, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < BIN_COUNT; ++i) {
        lv_obj_t* b = lv_btn_create(grid);
        lv_obj_set_size(b, 96, 48);
        lv_obj_set_style_radius(b, 6, 0);
        lv_obj_set_style_bg_color(b, C(i == localBin ? CLR_ROASTEK : 0xEEEEEE), 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, devBinSelectEvent, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        char bn[8];
        snprintf(bn, sizeof(bn), "仓%d", i + 1);
        lv_obj_t* bl = makeLabel(b, bn, &lv_font_chinese_14,
                                  C(i == localBin ? 0xFFFFFF : CLR_TEXT));
        lv_obj_center(bl);
    }
}

inline void logoEvent(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    Serial.println("[EVENT] 长按logo: 进入开发者模式(选择本机仓号)");
    showDevBinDialog();
}

// ===== 触摸 =====
inline uint16_t flipTouchAxis(uint16_t value, int maxValue) {
    return value >= maxValue ? 0 : static_cast<uint16_t>(maxValue - 1 - value);
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

// ===== 渲染flush =====
inline void displayFlush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors(reinterpret_cast<uint16_t*>(&color_p->full), w * h, true);
    tft.endWrite();
    lv_disp_flush_ready(disp);
}

// ===== 更新重量显示 =====
inline void updateWeights() {
    char buf[32];
    // 仓重大数字
    snprintf(buf, sizeof(buf), "%.1f", binWeights[localBin]);
    lv_label_set_text(binWeightLabel, buf);
    // 称重重量(左1/4)
    snprintf(buf, sizeof(buf), "%.1f", simCurrentWeight);
    lv_label_set_text(curWeightLabel, buf);
}

// ===== 更新在线/离线灯 + 警告框 =====
inline void setOnline(bool isOnline) {
    online = isOnline;
    updateConnLamp();
    if (online) {
        setButtonsEnabled(true);
    } else {
        setButtonsEnabled(false);
    }
}

inline void showCommAlarm() {
    if (warnBox) lv_obj_clear_flag(warnBox, LV_OBJ_FLAG_HIDDEN);
    if (warnCover) lv_obj_add_flag(warnCover, LV_OBJ_FLAG_HIDDEN);
}

inline void hideCommAlarm() {
    if (warnBox) lv_obj_add_flag(warnBox, LV_OBJ_FLAG_HIDDEN);
    if (warnCover) {
        lv_obj_clear_flag(warnCover, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(warnCover);
        lv_obj_invalidate(warnCover);
    }
}

// ===== 更新在线/离线灯 =====
inline void updateConnLamp() {
    if (!connLamp) return;
    if (online) {
        lv_obj_set_style_bg_color(connLamp, C(CLR_GREEN), 0);
        lv_obj_set_style_border_color(connLamp, C(CLR_GREEN), 0);
    } else {
        lv_obj_set_style_bg_color(connLamp, C(CLR_RED), 0);
        lv_obj_set_style_border_color(connLamp, C(CLR_RED), 0);
    }
}

// ===== 外部访问接口 (供main调用) =====
inline float Display_GetBinWeight() { return binWeights[localBin]; }
inline float Display_GetCurrentWeight() { return simCurrentWeight; }
inline uint8_t Display_GetLocalBinId() { return localBin + 1; }
inline void Display_SetCurrentWeight(float w) { simCurrentWeight = w; updateWeights(); }

// ESP-NOW 收到某仓状态变化 → 更新对应灯 + binOnline数组
inline void Display_OnBinStateChange(uint8_t binId, bool isBinOnline, float binWeight, float currentWeight) {
    if (binId < 1 || binId > BIN_COUNT) return;
    uint8_t idx = binId - 1;
    binOnline[idx] = isBinOnline;
    // 同步该仓重量(便于编辑面板等其他用途)
    if (idx != localBin) binWeights[idx] = binWeight;

    // 通信告警按“有没有收到有效在线心跳”判断：
    // 收到任意仓上线，都说明 ESP-NOW 通信已恢复，必须立刻隐藏左上告警并把大灯转绿。
    if (isBinOnline) {
        persistentError = false;
        ::online = true;
        commRecoverPending = true;
    } else if (idx == localBin) {
        // 只有本机仓被明确置离线时，才把大灯转红；远端单仓离线只影响6仓小灯。
        persistentError = true;
        ::online = false;
        ::setOnline(false);
        showCommAlarm();
    }
    Serial.printf("[Display] 仓%d %s (重量%.1f)\n", binId, isBinOnline ? "上线" : "离线", binWeight);
    updateBinDots();
}

// ESP-NOW 收到仓重同步包 → 更新本地仓重
inline void Display_OnBinWeightSync(uint8_t binId, float weight) {
    if (binId < 1 || binId > BIN_COUNT) return;
    uint8_t idx = binId - 1;
    binWeights[idx] = weight;
    nvsSaveBinWeights();
    if (idx == localBin) updateWeights();
    Serial.printf("[Display] 仓%d 重量同步: %.1f kg\n", binId, weight);
}

// ESP-NOW 全静默状态变化 (断网/恢复)
// 断网: 大灯变红 + 6个状态灯全灰 + 禁用按钮
// 恢复: 恢复正常 (后续心跳会重新点亮各仓)
inline void Display_OnSilence(bool silent) {
    Serial.printf("[Display] 全静默: %s\n", silent ? "断网" : "恢复");
    if (silent) {
        persistentError = true;
        // 大灯红 + 禁用按钮
        setOnline(false);
        showCommAlarm();
        // 6个状态灯全灰
        for (uint8_t i = 0; i < BIN_COUNT; ++i) binOnline[i] = false;
        updateBinDots();
    } else {
        persistentError = false;
        commRecoverPending = true;
        if (localBin < BIN_COUNT) binOnline[localBin] = true;
        setOnline(true);
        hideCommAlarm();
        updateBinDots();
    }
}

// ===== 更新6仓状态灯 =====
inline void updateBinDots() {
    for (uint8_t i = 0; i < BIN_COUNT; ++i) {
        if (!statusDots[i]) continue;
        // 在线=绿色, 离线=灰色; 本机仓高亮加玫瑰红边框
        lv_obj_set_style_bg_color(statusDots[i], C(binOnline[i] ? CLR_GREEN : CLR_GRAY), 0);
        if (i == localBin) {
            lv_obj_set_style_border_color(statusDots[i], C(CLR_ROASTEK), 0);
            lv_obj_set_style_border_width(statusDots[i], 3, 0);
        } else {
            lv_obj_set_style_border_width(statusDots[i], 0, 0);
        }
    }
}

// ===== 构建主页 =====
inline void buildHome() {
    root = lv_scr_act();
    lv_obj_set_style_bg_color(root, C(CLR_WHITE), 0);   // 背景全白
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    // --- 顶栏 (高60) ---
    lv_obj_t* top = lv_obj_create(root);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, SCREEN_WIDTH, 60);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(top, C(CLR_WHITE), 0);    // 顶栏白
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    // logo (左, 原始尺寸96x53)
    lv_obj_t* logo = lv_img_create(top);
    lv_img_set_src(logo, &logo_roastek);
    lv_obj_align(logo, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_add_flag(logo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(logo, logoEvent, LV_EVENT_LONG_PRESSED, nullptr);

    // 仓号文字 (logo右侧)
    char binTitle[8];
    snprintf(binTitle, sizeof(binTitle), "仓%d", localBin + 1);
    binLabel = makeLabel(top, binTitle, &lv_font_chinese_14, C(CLR_TEXT));
    lv_obj_align(binLabel, LV_ALIGN_LEFT_MID, 108, 0);

    // 在线/离线 大圆灯 (仓号右侧)
    connLamp = lv_obj_create(top);
    lv_obj_remove_style_all(connLamp);
    lv_obj_set_size(connLamp, 29, 29);
    lv_obj_align(connLamp, LV_ALIGN_LEFT_MID, 148, 0);
    lv_obj_set_style_radius(connLamp, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(connLamp, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(connLamp, 2, 0);
    updateConnLamp();

    // 6仓状态灯 (居中偏右, 左到右1-6)
    lv_obj_t* dots = lv_obj_create(top);
    lv_obj_remove_style_all(dots);
    lv_obj_set_size(dots, 168, 26);
    lv_obj_align(dots, LV_ALIGN_LEFT_MID, 208, 0);
    lv_obj_set_style_bg_opa(dots, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dots, 0, 0);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (uint8_t i = 0; i < BIN_COUNT; ++i) {
        statusDots[i] = lv_obj_create(dots);
        lv_obj_remove_style_all(statusDots[i]);
        lv_obj_set_size(statusDots[i], 18, 18);
        lv_obj_set_style_radius(statusDots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(statusDots[i], LV_OPA_COVER, 0);
    }
    updateBinDots();

    // 编辑按钮 (右上)
    btnEdit = makeButton(top, "编辑");
    lv_obj_set_size(btnEdit, 64, 36);
    lv_obj_align(btnEdit, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_event_cb(btnEdit, buttonEvent, LV_EVENT_CLICKED, const_cast<char*>("edit"));

    // --- 中间区: 左1/4称重重量 + 右3/4仓重大数字 (占满,与编辑按钮齐平) ---
    // 屏幕480宽: 左边120 (1/4) x=0..120, 右边360 (3/4) x=120..480
    lv_obj_t* middle = lv_obj_create(root);
    lv_obj_remove_style_all(middle);
    lv_obj_set_size(middle, SCREEN_WIDTH, 190);
    lv_obj_align(middle, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_bg_color(middle, C(CLR_WHITE), 0);  // 中间白
    lv_obj_set_style_bg_opa(middle, LV_OPA_COVER, 0);
    lv_obj_clear_flag(middle, LV_OBJ_FLAG_SCROLLABLE);

    // 离线警告框 (左上角向右延伸, 默认隐藏, 离线时显示)
    warnBox = lv_obj_create(middle);
    lv_obj_remove_style_all(warnBox);
    lv_obj_set_size(warnBox, 170, 74);
    lv_obj_align(warnBox, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_obj_set_style_bg_color(warnBox, C(CLR_WARN_YELLOW), 0);
    lv_obj_set_style_bg_opa(warnBox, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(warnBox, 8, 0);
    lv_obj_set_style_border_width(warnBox, 4, 0);
    lv_obj_set_style_border_color(warnBox, C(CLR_RED), 0);
    lv_obj_clear_flag(warnBox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(warnBox, LV_OBJ_FLAG_HIDDEN);  // 默认隐藏
    lv_obj_t* warnLbl = makeLabel(warnBox, "!\n离线", &lv_font_chinese_14, C(CLR_RED));
    lv_obj_set_style_text_align(warnLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(warnLbl);

    // 在线时不只隐藏告警框，还用白色块覆盖同一区域，强制刷新掉可能的屏幕残影。
    warnCover = lv_obj_create(middle);
    lv_obj_remove_style_all(warnCover);
    lv_obj_set_size(warnCover, 170, 74);
    lv_obj_align(warnCover, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_obj_set_style_bg_color(warnCover, C(CLR_WHITE), 0);
    lv_obj_set_style_bg_opa(warnCover, LV_OPA_COVER, 0);
    lv_obj_clear_flag(warnCover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(warnCover);

    // === 左侧: 称重重量 (占满左1/4, 宽120) 居中显示 ===
    curWeightLabel = makeLabel(middle, "--", &lv_font_montserrat_48, C(CLR_TEXT));
    // 称重在警告框下方,垂直居中偏下
    lv_obj_align(curWeightLabel, LV_ALIGN_TOP_LEFT, 30, 95);

    curKgLabel = makeLabel(middle, "kg", &lv_font_montserrat_24, C(CLR_MUTED));
    lv_obj_align_to(curKgLabel, curWeightLabel, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

    // === 右侧: 仓重大数字 (占满右3/4, x=120..480) ===
    // 130px大数字,居中在右3/4区域; kg在下方缩小
    binWeightLabel = makeLabel(middle, "--", &lv_font_numbers_130, C(CLR_TEXT));
    // 仓重大数字: 右移 + 下移 (不加zoom避免裁剪)
    lv_obj_align(binWeightLabel, LV_ALIGN_TOP_MID, 78, 35);

    kgLabel = makeLabel(middle, "kg", &lv_font_montserrat_24, C(CLR_MUTED));
    // kg在大数字下方居中
    lv_obj_align(kgLabel, LV_ALIGN_TOP_MID, 60, 150);

    // --- 底部 (只放加大加高的上料/下料按钮, 去掉消息提示) ---
    lv_obj_t* bottom = lv_obj_create(root);
    lv_obj_remove_style_all(bottom);
    lv_obj_set_size(bottom, SCREEN_WIDTH, 70);
    lv_obj_align(bottom, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bottom, C(CLR_WHITE), 0);  // 底部白
    lv_obj_set_style_bg_opa(bottom, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(bottom, 4, 0);
    lv_obj_clear_flag(bottom, LV_OBJ_FLAG_SCROLLABLE);

    // 按钮区 (上料/下料两个大按钮并排, 加大加高)
    lv_obj_t* btnRow = lv_obj_create(bottom);
    lv_obj_remove_style_all(btnRow);
    lv_obj_set_size(btnRow, SCREEN_WIDTH - 8, 62);
    lv_obj_align(btnRow, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    btnLoad = lv_btn_create(btnRow);
    lv_obj_set_size(btnLoad, 230, 58);
    lv_obj_set_style_radius(btnLoad, 8, 0);
    lv_obj_set_style_bg_color(btnLoad, C(CLR_ROASTEK), 0);
    lv_obj_set_style_bg_opa(btnLoad, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btnLoad, 0, 0);
    lv_obj_set_style_shadow_width(btnLoad, 0, 0);
    lv_obj_add_event_cb(btnLoad, buttonEvent, LV_EVENT_CLICKED, const_cast<char*>("load"));
    lv_obj_t* feedLbl = makeLabel(btnLoad, "上料完毕", &lv_font_chinese_14, lv_color_white());
    lv_obj_center(feedLbl);

    btnUnload = lv_btn_create(btnRow);
    lv_obj_set_size(btnUnload, 230, 58);
    lv_obj_set_style_radius(btnUnload, 8, 0);
    lv_obj_set_style_bg_color(btnUnload, C(CLR_ROASTEK), 0);
    lv_obj_set_style_bg_opa(btnUnload, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btnUnload, 0, 0);
    lv_obj_set_style_shadow_width(btnUnload, 0, 0);
    lv_obj_add_event_cb(btnUnload, buttonEvent, LV_EVENT_CLICKED, const_cast<char*>("unload"));
    lv_obj_t* dischLbl = makeLabel(btnUnload, "下料完毕", &lv_font_chinese_14, lv_color_white());
    lv_obj_center(dischLbl);

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
    lv_disp_draw_buf_init(&drawBuf, lvBuf1, nullptr, SCREEN_WIDTH * 10);
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

    nvsLoadBinWeights();  // 恢复上次断电前的数据
    buildHome();
    EspnowMesh_SetWeightSyncCallback(Display_OnBinWeightSync);  // 注册仓重同步回调
    EspnowMesh_SetSilenceCallback(Display_OnSilence);           // 注册全静默回调
    Serial.println("[Display] M1 unified UI initialized");
}

inline void Display_Loop() {
    bool anyBinOnline = false;
    for (uint8_t i = 0; i < BIN_COUNT; ++i) {
        if (binOnline[i]) {
            anyBinOnline = true;
            break;
        }
    }

    if (commRecoverPending || anyBinOnline) {
        commRecoverPending = false;
        persistentError = false;
        setOnline(true);
        hideCommAlarm();
        updateBinDots();
        static uint32_t lastRecoverLogMs = 0;
        if (millis() - lastRecoverLogMs > 3000) {
            lastRecoverLogMs = millis();
            Serial.println("[Display] 通信恢复: 已隐藏离线告警");
        }
    }
    // 兜底：通信/在线状态已恢复时，任何残留的黄色告警框都必须被压掉。
    // 告警框只允许 showCommAlarm() 主动显示，不再跟普通灯状态混在一起。
    if ((online || anyBinOnline) && warnBox) {
        lv_obj_add_flag(warnBox, LV_OBJ_FLAG_HIDDEN);
    }
    lv_timer_handler();
}

// ===== 启动自测 =====
inline void Display_SelfTest() {
    Serial.println("[SELFTEST] === 开始 ===");
    bool allPass = true;

    simCurrentWeight = 10.0f;
    binWeights[0] = 50.0f;
    Serial.printf("[SELFTEST] 初始: 仓重=%.1f 当前=%.1f\n", binWeights[0], simCurrentWeight);

    float before = binWeights[0];
    binWeights[0] += simCurrentWeight;
    bool t1 = (binWeights[0] == 60.0f);
    Serial.printf("[SELFTEST] 上料: %.1f += %.1f => %.1f  %s\n",
                  before, simCurrentWeight, binWeights[0], t1 ? "PASS" : "FAIL");
    allPass &= t1;

    before = binWeights[0];
    bool canUnload = binWeights[0] >= simCurrentWeight;
    if (canUnload) binWeights[0] -= simCurrentWeight;
    bool t2 = (binWeights[0] == 50.0f) && canUnload;
    Serial.printf("[SELFTEST] 下料: %.1f -= %.1f => %.1f  %s\n",
                  before, simCurrentWeight, binWeights[0], t2 ? "PASS" : "FAIL");
    allPass &= t2;

    binWeights[0] = 3.0f;
    before = binWeights[0];
    canUnload = binWeights[0] >= simCurrentWeight;
    if (canUnload) binWeights[0] -= simCurrentWeight;
    bool t3 = (!canUnload) && (binWeights[0] == 3.0f);
    Serial.printf("[SELFTEST] 下料不足保护: 仓重 %.1f < 当前 %.1f, 拒绝, 仓重仍 %.1f  %s\n",
                  before, simCurrentWeight, binWeights[0], t3 ? "PASS" : "FAIL");
    allPass &= t3;

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

    binWeights[0] = 45.0f;
    updateWeights();
    showMessage("仓1 在线", false, false);

    Serial.printf("[SELFTEST] === %s ===\n", allPass ? "ALL PASS" : "SOME FAILED");
}
