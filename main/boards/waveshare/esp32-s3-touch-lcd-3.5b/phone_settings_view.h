#pragma once

#include <lvgl.h>
#include <array>
#include "call_records.h"
#include "gateway_manager.h"
#include "phone_ai_state.h"

// Board-owned 480x320 settings. All methods run while the display is locked.
class PhoneSettingsView {
public:
    explicit PhoneSettingsView(lv_obj_t* screen, const lv_font_t* content_font = nullptr);
    ~PhoneSettingsView();
    bool Command(const char* command);
    void AddTranscript(bool user, const char* text, const lv_font_t* font);
    void SetContentFont(const lv_font_t* font);

private:
    enum class Page { Closed, Settings, Search, Replace, Provision, Result, Info, Records, Detail };
    enum class Action {
        Open,
        Home,
        Records,
        Rules,
        Search,
        Rescan,
        Close,
        Cancel,
        Select,
        Replace,
        Reprovision,
        Done,
        Answer,
        Reject,
        Latest,
        Pause,
        RecordSelect,
        RecentSelect,
        RecordsPrevious,
        RecordsNext,
        DetailBack,
        TranscriptPrevious,
        TranscriptNext,
        PlaybackToggle
    };
    struct Target {
        PhoneSettingsView* view;
        Action action;
        unsigned index;
        lv_obj_t* object;
    };
    std::array<Target, 64> targets_{};
    unsigned target_count_ = 0;
    Page page_ = Page::Closed;
    lv_obj_t *entry_, *root_, *shade_, *dialog_, *title_, *summary_, *list_, *message_, *target_,
        *countdown_, *cancel_, *retry_, *accept_, *close_, *gateway_, *link_, *phone_, *ai_,
        *clock_, *reprovision_, *key_;
    std::array<lv_obj_t*, GATEWAY_UI_MAX_PEERS> rows_{}, row_ids_{}, row_rssi_{}, row_buttons_{};
    lv_timer_t* timer_ = nullptr;
    gateway_ui_status_t status_{};
    char selected_[16]{};
    uint32_t scan_before_ = 0, started_ = 0, handshake_since_ = 0;
    uint32_t lvgl_stack_free_ = 0;
    bool awaiting_scan_ = false, operation_ = false, accepted_ = false, recovery_ = false;
    lv_obj_t *phone_root_, *phone_title_, *phone_number_, *phone_state_, *phone_time_,
        *phone_links_, *phone_notice_, *phone_transcripts_, *phone_answer_, *phone_reject_,
        *phone_latest_, *phone_pause_, *phone_nav_;
    uint32_t phone_call_id_ = 0;
    phone_status_t cached_phone_{};
    uint32_t cached_phone_at_ = 0, cached_duration_ = 0;
    bool following_ = true, scrolling_ = false;
    CallRecordView* records_ = nullptr;
    const lv_font_t* content_font_ = nullptr;
    uint32_t record_revision_ = UINT32_MAX;
    lv_obj_t *storage_text_ = nullptr, *record_root_ = nullptr, *detail_root_ = nullptr,
             *record_status_ = nullptr, *detail_info_ = nullptr, *detail_summary_ = nullptr,
             *detail_text_ = nullptr, *playback_button_ = nullptr, *playback_progress_ = nullptr,
             *playback_time_ = nullptr, *detail_notice_ = nullptr;
    lv_obj_t* record_rows_[3]{};
    lv_obj_t* recent_rows_[2]{};
    void CreateRecords(lv_obj_t* screen);
    void RefreshRecords();
    static void Event(lv_event_t* event);
    static void Tick(lv_timer_t* timer);
    void Act(Action action, unsigned index = 0);
    void Refresh();
    void RefreshPhone();
    void Show(Page page);
    void StartScan();
    void StartProvision(bool recovery);
    void Result(const char* title, const char* message);
    bool Accessible(lv_obj_t* object) const;
    lv_obj_t* Button(lv_obj_t* parent, const char* text, int x, int y, int w, Action action,
                     unsigned index = 0);
};
