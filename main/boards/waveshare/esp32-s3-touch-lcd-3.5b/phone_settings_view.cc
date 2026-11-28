#include "phone_settings_view.h"
#include "call_record_format.h"
#include "call_record_memory.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include "application.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

#if CONFIG_GATEWAY_MANAGER
extern const lv_font_t phone_settings_font;

namespace {
constexpr uint32_t kBg = 0x081321, kCard = 0x102337, kEdge = 0x23445B, kCyan = 0x54D5E8,
                   kWhite = 0xE4F1F6, kMuted = 0x92A9BC;
void Visible(lv_obj_t* object, bool visible) {
    if (visible)
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}
void Text(lv_obj_t* label, const char* text) {
    if (strcmp(lv_label_get_text(label), text))
        lv_label_set_text(label, text);
}
lv_obj_t* Box(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color) {
    auto o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 10, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(kEdge), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}
lv_obj_t* Label(lv_obj_t* parent, const char* text, int x, int y, int w, uint32_t color = kWhite) {
    auto o = lv_label_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_width(o, w);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_label_set_text(o, text);
    return o;
}
void Enable(lv_obj_t* object, bool enabled) {
    if (enabled)
        lv_obj_remove_state(object, LV_STATE_DISABLED);
    else
        lv_obj_add_state(object, LV_STATE_DISABLED);
}
const char* Error(const char* error) {
    if (!strcmp(error, "busy_call"))
        return "通话或音频正在使用，请挂断后重试。";
    if (!strcmp(error, "s3_wifi_offline"))
        return "S3 尚未联网，请先连接路由器。";
    if (!strcmp(error, "provision_timeout"))
        return "配网超时，请确认网关已上电后重试。";
    if (!strcmp(error, "button_confirmation_timeout"))
        return "按键确认超时，请重新连接后及时短按 BOOT。";
    if (!strcmp(error, "ble_disconnected"))
        return "配置连接已断开，请靠近网关后重试。";
    if (!strcmp(error, "remote_provision_failed"))
        return "网关联网失败，请检查 Wi-Fi 后重新配网。";
    if (!strcmp(error, "recovery_gateway_not_found"))
        return "未找到已绑定网关，请检查电源和配网窗口。";
    if (!strcmp(error, "ble_closing_retry"))
        return "上一连接正在关闭，请稍后重试。";
    return "操作未完成，请检查网关状态后重试。";
}
const char* RecordError(const char* error) {
    if (!strcmp(error, "call_state_lost"))
        return "通话连接曾中断，录音不完整";
    if (!strcmp(error, "audio_gaps") || !strcmp(error, "missing_media"))
        return "录音有缺口，请核对文字和声音";
    if (!strcmp(error, "interrupted_save"))
        return "上次保存被中断";
    if (!strcmp(error, "storage_full"))
        return "SD 卡空间不足";
    if (!strcmp(error, "storage_unavailable"))
        return "SD 卡不可用，请检查插卡";
    if (!strcmp(error, "transcript_drops") || !strcmp(error, "transcript_read_failed"))
        return "部分文字缺失，请核对录音";
    if (!strcmp(error, "audio_missing_or_read_failed") || !strcmp(error, "audio_read_failed") ||
        !strcmp(error, "audio_seek_failed"))
        return "录音读取失败，请检查 SD 卡";
    return "记录处理未完成，请检查 SD 卡";
}
}  // namespace

lv_obj_t* PhoneSettingsView::Button(lv_obj_t* parent, const char* text, int x, int y, int w,
                                    Action action, unsigned index) {
    auto button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, w, 44);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x163D50), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(kCyan), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(button, LV_OPA_40, LV_STATE_DISABLED);
    auto label = Label(button, text, 0, 0, w, kCyan);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    auto& target = targets_.at(target_count_++);
    target = {this, action, index, button};
    lv_obj_add_event_cb(button, Event, LV_EVENT_CLICKED, &target);
    return button;
}

PhoneSettingsView::PhoneSettingsView(lv_obj_t* screen, const lv_font_t* content_font)
    : content_font_(content_font) {
    entry_ = Button(screen, "设置", 386, 34, 82, Action::Open);
    lv_obj_set_style_text_font(entry_, &phone_settings_font, 0);
    root_ = Box(screen, 0, 0, 480, 320, kBg);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_text_font(root_, &phone_settings_font, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    Label(root_, "设置", 16, 12, 70, kCyan);
    char version[64];
    snprintf(version, sizeof(version), "固件 v%s", esp_app_get_description()->version);
    Label(root_, version, 128, 12, 170, kMuted);
    clock_ = Label(root_, "时间未同步", 306, 12, 160, kMuted);
    auto line = Box(root_, 16, 37, 448, 1, kCyan);
    lv_obj_set_style_border_width(line, 0, 0);
    auto gateway_card = Box(root_, 16, 49, 252, 211, kCard);
    Label(gateway_card, "电话网关", 12, 10, 220, kCyan);
    gateway_ = Label(gateway_card, "尚未绑定", 12, 38, 228);
    link_ = Label(gateway_card, "双板连接：未连接", 12, 66, 228);
    phone_ = Label(gateway_card, "手机 HFP：待网关就绪", 12, 92, 228);
    ai_ = Label(gateway_card, "AI 服务：未连接", 12, 118, 228);
    Button(gateway_card, "搜索电话网关", 12, 153, 228, Action::Search);
    auto storage = Box(root_, 280, 49, 184, 104, kCard);
    Label(storage, "SD 卡与录音", 12, 10, 160, kCyan);
    storage_text_ = Label(storage, "正在检查 SD 卡", 12, 32, 160, kMuted);
    auto rules = Box(root_, 280, 163, 184, 97, kCard);
    Label(rules, "接听规则", 12, 8, 160, kCyan);
    Button(rules, "语音设置规则", 10, 38, 164, Action::Rules);
    Button(root_, "待机", 16, 276, 144, Action::Home);
    Button(root_, "记录", 168, 276, 144, Action::Records);
    Button(root_, "设置", 320, 276, 144, Action::Open);

    shade_ = Box(screen, 0, 0, 480, 320, 0x000000);
    lv_obj_set_style_text_font(shade_, &phone_settings_font, 0);
    lv_obj_set_style_bg_opa(shade_, LV_OPA_70, 0);
    lv_obj_set_style_radius(shade_, 0, 0);
    dialog_ = Box(shade_, 24, 16, 432, 288, kBg);
    title_ = Label(dialog_, "搜索电话网关", 16, 14, 352, kCyan);
    close_ = Button(dialog_, "X", 376, 0, 44, Action::Close);
    summary_ = Label(dialog_, "", 16, 46, 400, kMuted);
    list_ = Box(dialog_, 16, 74, 400, 114, kBg);
    lv_obj_add_flag(list_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    for (unsigned i = 0; i < rows_.size(); ++i) {
        rows_[i] = Box(list_, 0, i * 62, 390, 58, kCard);
        row_ids_[i] = Label(rows_[i], "", 8, 7, 265);
        row_rssi_[i] = Label(rows_[i], "", 8, 31, 265, kMuted);
        row_buttons_[i] = Button(rows_[i], "连接", 294, 7, 84, Action::Select, i);
    }
    target_ = Label(dialog_, "", 16, 75, 400, kCyan);
    key_ = Box(dialog_, 184, 104, 64, 48, kCard);
    Label(key_, "KEY1", 4, 14, 56, kCyan);
    message_ = Label(dialog_, "", 16, 154, 400);
    countdown_ = Label(dialog_, "", 16, 212, 400, kMuted);
    cancel_ = Button(dialog_, "取消", 16, 232, 124, Action::Cancel);
    retry_ = Button(dialog_, "重新搜索", 152, 232, 128, Action::Rescan);
    accept_ = Button(dialog_, "确认替换", 292, 232, 124, Action::Replace);
    reprovision_ = Button(dialog_, "重新配网", 280, 186, 136, Action::Reprovision);
    Show(Page::Closed);
    // A shared idle/incoming/call canvas keeps the left controls fixed while
    // only the bounded transcript list scrolls on the right.
    phone_root_ = Box(screen, 0, 0, 480, 320, kBg);
    lv_obj_set_style_text_font(phone_root_, &phone_settings_font, 0);
    lv_obj_set_style_radius(phone_root_, 0, 0);
    phone_title_ = Label(phone_root_, "电话助手", 12, 10, 148, kCyan);
    phone_number_ = Label(phone_root_, "待机", 12, 42, 144);
    phone_state_ = Label(phone_root_, "AI 尚未就绪", 12, 74, 144, kCyan);
    phone_time_ = Label(phone_root_, "时间未同步", 12, 104, 144, kMuted);
    phone_links_ = Label(phone_root_, "", 12, 134, 144, kMuted);
    phone_notice_ = Label(phone_root_, "", 176, 42, 288, kMuted);
    phone_transcripts_ = Box(phone_root_, 170, 78, 298, 178, kCard);
    lv_obj_add_flag(phone_transcripts_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(phone_transcripts_, LV_DIR_VER);
    lv_obj_set_flex_flow(phone_transcripts_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(phone_transcripts_, 8, 0);
    lv_obj_set_style_pad_row(phone_transcripts_, 12, 0);
    lv_obj_add_event_cb(
        phone_transcripts_,
        [](lv_event_t* e) {
            auto* view = static_cast<PhoneSettingsView*>(lv_event_get_user_data(e));
            if (!view->scrolling_) {
                view->following_ = false;
                Text(lv_obj_get_child(view->phone_pause_, 0), "继续跟随");
            }
        },
        LV_EVENT_SCROLL_BEGIN, this);
    phone_answer_ = Button(phone_root_, "AI 立即接听", 12, 194, 144, Action::Answer);
    phone_reject_ = Button(phone_root_, "拒接", 12, 246, 144, Action::Reject);
    phone_latest_ = Button(phone_root_, "回到最新", 320, 268, 148, Action::Latest);
    phone_pause_ = Button(phone_root_, "暂停跟随", 170, 268, 140, Action::Pause);
    phone_nav_ = Box(phone_root_, 0, 270, 480, 50, kBg);
    Button(phone_nav_, "待机", 12, 0, 144, Action::Home);
    Button(phone_nav_, "记录", 168, 0, 144, Action::Records);
    Button(phone_nav_, "设置", 324, 0, 144, Action::Open);
    CreateRecords(screen);
    timer_ = lv_timer_create(Tick, 250, this);
    RefreshPhone();
}

void PhoneSettingsView::CreateRecords(lv_obj_t* screen) {
    records_ = static_cast<CallRecordView*>(
        heap_caps_calloc(1, sizeof(CallRecordView), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (records_)
        new (records_) CallRecordView;
    else
        call_record::AllocationFailure("record_view", sizeof(CallRecordView),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    record_root_ = Box(screen, 0, 0, 480, 320, kBg);
    lv_obj_set_style_text_font(record_root_, &phone_settings_font, 0);
    Label(record_root_, "通话记录", 12, 8, 120, kCyan);
    record_status_ = Label(record_root_, "正在读取 SD 卡", 144, 8, 324, kMuted);
    for (unsigned i = 0; i < 3; ++i) {
        record_rows_[i] = Button(record_root_, "", 12, 40 + i * 72, 456, Action::RecordSelect, i);
        lv_obj_set_height(record_rows_[i], 66);
        auto* label = lv_obj_get_child(record_rows_[i], 0);
        lv_obj_set_align(label, LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(label, 10, 2);
        lv_obj_set_size(label, 432, 20);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        auto* state = Label(record_rows_[i], "", 10, 23, 432, kMuted);
        lv_obj_set_height(state, 18);
        lv_label_set_long_mode(state, LV_LABEL_LONG_DOT);
        auto* summary = Label(record_rows_[i], "", 10, 43, 432, kWhite);
        lv_obj_set_height(summary, 20);
        lv_label_set_long_mode(summary, LV_LABEL_LONG_DOT);
        if (content_font_)
            lv_obj_set_style_text_font(summary, content_font_, 0);
    }
    Button(record_root_, "待机", 12, 270, 104, Action::Home);
    Button(record_root_, "上一页", 124, 270, 104, Action::RecordsPrevious);
    Button(record_root_, "下一页", 236, 270, 104, Action::RecordsNext);
    Button(record_root_, "设置", 352, 270, 116, Action::Open);
    detail_root_ = Box(screen, 0, 0, 480, 320, kBg);
    lv_obj_set_style_text_font(detail_root_, &phone_settings_font, 0);
    Label(detail_root_, "通话详情", 12, 8, 144, kCyan);
    detail_notice_ = Label(detail_root_, "通话文字", 174, 8, 292, kMuted);
    detail_info_ = Label(detail_root_, "", 12, 36, 144);
    auto* summary = Box(detail_root_, 8, 132, 152, 60, kCard);
    lv_obj_add_flag(summary, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(summary, LV_DIR_VER);
    detail_summary_ = Label(summary, "", 6, 4, 138, kMuted);
    if (content_font_)
        lv_obj_set_style_text_font(detail_summary_, content_font_, 0);
    playback_button_ = Button(detail_root_, "播放录音", 12, 198, 144, Action::PlaybackToggle);
    playback_progress_ = lv_slider_create(detail_root_);
    lv_obj_set_pos(playback_progress_, 20, 242);
    lv_obj_set_size(playback_progress_, 128, 10);
    lv_slider_set_range(playback_progress_, 0, 1000);
    lv_obj_add_event_cb(
        playback_progress_,
        [](lv_event_t* event) {
            auto* view = static_cast<PhoneSettingsView*>(lv_event_get_user_data(event));
            if (view->records_)
                CallRecords::Instance().Seek(
                    uint64_t(lv_slider_get_value(view->playback_progress_)) *
                    view->records_->playback_duration_ms / 1000);
        },
        LV_EVENT_RELEASED, this);
    playback_time_ = Label(detail_root_, "00:00 / 00:00", 12, 254, 144, kMuted);
    auto* text = Box(detail_root_, 170, 36, 298, 226, kCard);
    lv_obj_add_flag(text, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(text, LV_DIR_VER);
    detail_text_ = Label(text, "", 8, 6, 276);
    if (content_font_)
        lv_obj_set_style_text_font(detail_text_, content_font_, 0);
    Button(detail_root_, "返回列表", 12, 276, 144, Action::DetailBack);
    Button(detail_root_, "前段文字", 174, 270, 140, Action::TranscriptPrevious);
    Button(detail_root_, "后段文字", 326, 270, 140, Action::TranscriptNext);
    for (unsigned i = 0; i < 2; ++i) {
        recent_rows_[i] = Button(phone_root_, "", 172, 82 + i * 88, 296, Action::RecentSelect, i);
        lv_obj_set_height(recent_rows_[i], 80);
        auto* label = lv_obj_get_child(recent_rows_[i], 0);
        lv_obj_set_align(label, LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(label, 8, 4);
        lv_obj_set_size(label, 276, 20);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        auto* state = Label(recent_rows_[i], "", 8, 28, 276, kMuted);
        lv_obj_set_height(state, 18);
        lv_label_set_long_mode(state, LV_LABEL_LONG_DOT);
        auto* summary = Label(recent_rows_[i], "", 8, 52, 276, kWhite);
        lv_obj_set_height(summary, 20);
        lv_label_set_long_mode(summary, LV_LABEL_LONG_DOT);
        if (content_font_)
            lv_obj_set_style_text_font(summary, content_font_, 0);
    }
    Visible(record_root_, false);
    Visible(detail_root_, false);
}

void PhoneSettingsView::SetContentFont(const lv_font_t* font) {
    content_font_ = font ? font : &phone_settings_font;
    for (auto* row : record_rows_)
        lv_obj_set_style_text_font(lv_obj_get_child(row, 2), content_font_, 0);
    for (auto* row : recent_rows_)
        lv_obj_set_style_text_font(lv_obj_get_child(row, 2), content_font_, 0);
    lv_obj_set_style_text_font(detail_summary_, content_font_, 0);
    lv_obj_set_style_text_font(detail_text_, content_font_, 0);
    for (unsigned i = 0; i < lv_obj_get_child_count(phone_transcripts_); ++i)
        lv_obj_set_style_text_font(lv_obj_get_child(phone_transcripts_, i), content_font_, 0);
}

void PhoneSettingsView::RefreshRecords() {
    if (!records_)
        return;
    CallRecords::Instance().Snapshot(*records_);
    Visible(record_root_, page_ == Page::Records);
    Visible(detail_root_, page_ == Page::Detail);
    const bool idle = page_ == Page::Closed && cached_phone_.phase != PHONE_ACTIVE &&
                      cached_phone_.phase != PHONE_INCOMING &&
                      cached_phone_.phase != PHONE_OUTGOING;
    auto save_text = [](CallSaveState s) {
        switch (s) {
            case CallSaveState::Recording:
                return "双向录音写入中";
            case CallSaveState::Saving:
                return "录音保存中";
            case CallSaveState::Saved:
                return "已保存到 SD 卡";
            case CallSaveState::Incomplete:
                return "录音不完整";
            case CallSaveState::Failed:
                return "录音保存失败";
            case CallSaveState::Ready:
                return "SD 卡可用";
            default:
                return "SD 卡不可用，未录音";
        }
    };
    char status[192];
    if (records_->mounted)
        snprintf(status, sizeof(status), "剩余 %llu MB\n%s", records_->free_bytes / (1024 * 1024),
                 save_text(records_->save));
    else
        snprintf(status, sizeof(status), "SD 卡不可用，请检查插卡");
    Text(storage_text_, status);
    if (cached_phone_.phase == PHONE_ACTIVE)
        Text(phone_notice_, save_text(records_->save));
    else if (idle && records_->save == CallSaveState::Saving)
        Text(phone_notice_, "录音保存中，请勿断电");
    else if (idle && !records_->mounted)
        Text(phone_notice_, "SD 卡不可用，无法读取记录");
    else if (idle && !records_->total)
        Text(phone_notice_, "暂无已接听电话");
    else if (idle)
        Text(phone_notice_, "最近已接听电话");
    auto describe = [&](lv_obj_t* row, const CallRecordInfo& r) {
        char when[32] = "时间未同步";
        if (r.started_at) {
            time_t local = r.started_at + 8 * 3600;
            tm date{};
            gmtime_r(&local, &date);
            strftime(when, sizeof(when), "%m/%d %H:%M", &date);
        }
        const char* summary = r.summary[0]                         ? r.summary
                              : !strcmp(r.summary_state, "failed") ? "概要生成失败"
                                                                   : "概要生成中";
        char out[192];
        snprintf(out, sizeof(out), "%s  %s  %02lu:%02lu",
                 strcmp(r.caller, "unknown") ? r.caller : "未知来电", when,
                 (unsigned long)(r.duration_ms / 60000),
                 (unsigned long)(r.duration_ms / 1000 % 60));
        Text(lv_obj_get_child(row, 0), out);
        snprintf(out, sizeof(out), "%s · %s", r.taken_over ? "人工接管" : "AI 接听",
                 save_text(r.save));
        Text(lv_obj_get_child(row, 1), out);
        Text(lv_obj_get_child(row, 2), summary);
    };
    for (unsigned i = 0; i < 2; ++i) {
        Visible(recent_rows_[i], idle && records_->recent[i].id[0]);
        describe(recent_rows_[i], records_->recent[i]);
    }
    if (page_ == Page::Records) {
        if (!records_->mounted || records_->storage_error[0])
            snprintf(status, sizeof(status), "SD 读取失败，请检查插卡");
        else if (records_->loading)
            snprintf(status, sizeof(status), "正在读取…");
        else if (!records_->total)
            snprintf(status, sizeof(status), "暂无已接听电话");
        else
            snprintf(status, sizeof(status), "共 %lu 通 · 剩余 %llu MB",
                     (unsigned long)records_->total, records_->free_bytes / (1024 * 1024));
        Text(record_status_, status);
        for (unsigned i = 0; i < 3; ++i) {
            Visible(record_rows_[i], records_->mounted && i < records_->count);
            Enable(record_rows_[i], !records_->loading);
            describe(record_rows_[i], records_->rows[i]);
        }
    }
    if (page_ == Page::Detail) {
        const auto& r = records_->detail;
        if (record_revision_ != records_->revision) {
            char when[32] = "时间未同步";
            if (r.started_at) {
                time_t local = r.started_at + 8 * 3600;
                tm date{};
                gmtime_r(&local, &date);
                strftime(when, sizeof(when), "%m/%d %H:%M", &date);
            }
            snprintf(status, sizeof(status), "%s\n%s\n%02lu:%02lu · %s\n%s",
                     strcmp(r.caller, "unknown") ? r.caller : "未知来电", when,
                     (unsigned long)(r.duration_ms / 60000),
                     (unsigned long)(r.duration_ms / 1000 % 60),
                     r.taken_over ? "人工接管" : "AI 接听", save_text(r.save));
            Text(detail_info_, status);
            Text(detail_summary_, r.summary[0]                         ? r.summary
                                  : !strcmp(r.summary_state, "failed") ? "概要生成失败"
                                                                       : "概要生成中");
            Text(detail_text_, records_->transcript[0] ? records_->transcript
                               : r.error[0]            ? "文字读取失败，请查看状态"
                                                       : "暂无已确定文字");
            Text(detail_notice_, records_->loading ? "正在读取…"
                                 : r.error[0]      ? RecordError(r.error)
                                 : !strcmp(r.transcription_state, "complete")
                                     ? "完整通话转写"
                                     : "部分转写 · 尚未完成补识别");
            record_revision_ = records_->revision;
        }
        Enable(playback_button_, r.audio_available && !records_->loading);
        Text(lv_obj_get_child(playback_button_, 0),
             records_->playback == CallPlaybackState::Playing  ? "暂停播放"
             : records_->playback == CallPlaybackState::Paused ? "继续播放"
                                                               : "播放录音");
        if (records_->playback == CallPlaybackState::Failed)
            Text(detail_notice_, "录音读取失败，请检查 SD 卡");
        if (!lv_slider_is_dragged(playback_progress_))
            lv_slider_set_value(
                playback_progress_,
                records_->playback_duration_ms
                    ? uint64_t(records_->playback_ms) * 1000 / records_->playback_duration_ms
                    : 0,
                LV_ANIM_OFF);
        snprintf(status, sizeof(status), "%02lu:%02lu / %02lu:%02lu",
                 (unsigned long)(records_->playback_ms / 60000),
                 (unsigned long)(records_->playback_ms / 1000 % 60),
                 (unsigned long)(r.duration_ms / 60000),
                 (unsigned long)(r.duration_ms / 1000 % 60));
        Text(playback_time_, status);
    }
}

PhoneSettingsView::~PhoneSettingsView() {
    if (timer_)
        lv_timer_delete(timer_);
    lv_obj_delete(shade_);
    lv_obj_delete(root_);
    lv_obj_delete(entry_);
    lv_obj_delete(phone_root_);
    lv_obj_delete(record_root_);
    lv_obj_delete(detail_root_);
    heap_caps_free(records_);
}
void PhoneSettingsView::Event(lv_event_t* event) {
    auto* t = static_cast<Target*>(lv_event_get_user_data(event));
    t->view->Act(t->action, t->index);
}
void PhoneSettingsView::Tick(lv_timer_t* timer) {
    auto* view = static_cast<PhoneSettingsView*>(lv_timer_get_user_data(timer));
    view->Refresh();
    view->lvgl_stack_free_ = uxTaskGetStackHighWaterMark(nullptr);
}
void PhoneSettingsView::Show(Page page) {
    if (page_ == Page::Detail && page != Page::Detail)
        CallRecords::Instance().StopPlayback();
    page_ = page;
    Visible(entry_, page == Page::Closed);
    Visible(root_, page != Page::Closed && page != Page::Records && page != Page::Detail);
    const bool modal = page != Page::Closed && page != Page::Settings && page != Page::Records &&
                       page != Page::Detail;
    Visible(shade_, modal);
    Visible(list_, page == Page::Search);
    Visible(target_, page == Page::Replace || page == Page::Provision);
    Visible(key_, page == Page::Provision);
    Visible(countdown_, page == Page::Provision);
    Visible(retry_, page == Page::Search || page == Page::Result);
    Visible(accept_, page == Page::Replace);
    Visible(reprovision_, page == Page::Search && status_.bound);
    lv_obj_set_pos(message_, 16, page == Page::Search ? 192 : page == Page::Provision ? 156 : 112);
    lv_obj_set_height(message_, page == Page::Search ? 36 : page == Page::Provision ? 52 : 104);
    lv_obj_set_width(message_, page == Page::Search && status_.bound ? 250 : 400);
    lv_label_set_long_mode(message_, LV_LABEL_LONG_WRAP);
    if (page == Page::Search) {
        Text(title_, "搜索电话网关");
        Text(message_, "首次连接需短按 BOOT");
    }
    if (page == Page::Replace) {
        Text(title_, "替换当前电话网关？");
        Text(summary_, "当前绑定会在新网关配网成功后替换");
        Text(target_, selected_);
        Text(message_,
             "请确认目标设备。取消不会发送新的 Wi-Fi 配置；失败后请以设置页实际连接状态为准。");
    }
    if (page == Page::Provision) {
        Text(title_, recovery_ ? "已绑定网关重新配网" : "网关硬件确认");
        Text(summary_, "连接中，请保持两台设备上电");
        Text(target_, selected_);
    }
    Enable(cancel_, true);
    Enable(close_, true);
    Text(lv_obj_get_child(cancel_, 0),
         page == Page::Info || page == Page::Result ? "返回" : "取消");
}
void PhoneSettingsView::Result(const char* title, const char* message) {
    Show(Page::Result);
    Text(title_, title);
    Text(summary_,
         status_.ready && status_.remote_fresh ? "双板认证连接仍在线" : "双板连接当前未就绪");
    Text(message_, message);
}
void PhoneSettingsView::StartScan() {
    gateway_manager_get_ui_status(&status_);
    scan_before_ = status_.scan_serial;
    awaiting_scan_ = true;
    started_ = lv_tick_get();
    operation_ = false;
    Show(Page::Search);
    Text(summary_, "正在请求搜索…");
    for (auto row : rows_)
        Visible(row, false);
    if (!gateway_manager_command("gateway scan")) {
        awaiting_scan_ = false;
        Result("搜索请求失败", "网关服务尚未就绪，请稍后重试。");
    }
}
void PhoneSettingsView::StartProvision(bool recovery) {
    recovery_ = recovery;
    operation_ = true;
    accepted_ = false;
    started_ = lv_tick_get();
    scan_before_ = status_.scan_serial;
    handshake_since_ = 0;
    Show(Page::Provision);
    char command[64];
    if (recovery)
        snprintf(command, sizeof(command), "gateway reprovision");
    else
        snprintf(command, sizeof(command), "gateway provision %s", selected_);
    if (!gateway_manager_command(command)) {
        operation_ = false;
        Result("无法开始配网", "网关服务队列繁忙，请稍后重试。");
    }
}
void PhoneSettingsView::Act(Action action, unsigned index) {
    ESP_LOGI("phone_ui", "action=%u index=%u page=%u", unsigned(action), index, unsigned(page_));
    gateway_manager_get_ui_status(&status_);
    switch (action) {
        case Action::Open:
            Show(Page::Settings);
            break;
        case Action::Home:
            Show(Page::Closed);
            break;
        case Action::Search:
        case Action::Rescan:
            StartScan();
            break;
        case Action::Records:
            Show(Page::Records);
            CallRecords::Instance().RequestPage(0);
            break;
        case Action::Rules:
            Show(Page::Info);
            Text(title_, action == Action::Records ? "通话记录" : "语音设置规则");
            Text(summary_, "功能尚未接入");
            Text(message_, action == Action::Records
                               ? "录音与记录将在阶段五接入；当前没有可查询的通话记录。"
                               : "规则编辑将在阶段六接入；当前设置未改变。");
            break;
        case Action::RecordSelect:
        case Action::RecentSelect:
            if (records_ && index < (action == Action::RecentSelect ? 2U : records_->count)) {
                const auto& record = action == Action::RecentSelect ? records_->recent[index]
                                                                    : records_->rows[index];
                if (record.id[0]) {
                    Show(Page::Detail);
                    CallRecords::Instance().RequestDetail(record.id);
                }
            }
            break;
        case Action::RecordsPrevious:
            if (records_)
                CallRecords::Instance().RequestPage(records_->offset >= 3 ? records_->offset - 3
                                                                          : 0);
            break;
        case Action::RecordsNext:
            if (records_ && records_->offset + records_->count < records_->total)
                CallRecords::Instance().RequestPage(records_->offset + 3);
            break;
        case Action::DetailBack:
            Show(Page::Records);
            CallRecords::Instance().RequestPage(records_ ? records_->offset : 0);
            break;
        case Action::TranscriptPrevious:
        case Action::TranscriptNext:
            if (records_) {
                const auto offset = records_->transcript_offset;
                CallRecords::Instance().RequestDetail(
                    records_->detail.id,
                    action == Action::TranscriptNext
                        ? (records_->transcript_more ? records_->transcript_next : offset)
                        : records_->transcript_previous);
            }
            break;
        case Action::PlaybackToggle:
            if (records_ && records_->playback != CallPlaybackState::Playing &&
                records_->playback != CallPlaybackState::Paused)
                CallRecords::Instance().Play(records_->detail.id);
            else
                CallRecords::Instance().Pause();
            break;
        case Action::Select:
            if (index >= status_.count || status_.scanning || awaiting_scan_)
                break;
            snprintf(selected_, sizeof(selected_), "%s", status_.peers[index].id);
            if (status_.bound && strcmp(selected_, status_.peer_id))
                Show(Page::Replace);
            else
                StartProvision(false);
            break;
        case Action::Replace:
            StartProvision(false);
            break;
        case Action::Reprovision:
            snprintf(selected_, sizeof(selected_), "%s", status_.peer_id);
            StartProvision(true);
            break;
        case Action::Cancel:
        case Action::Close:
            if (operation_ && status_.config_sent)
                break;
            if (operation_ || status_.scanning || awaiting_scan_) {
                if (!gateway_manager_command("gateway cancel"))
                    break;
            }
            awaiting_scan_ = operation_ = false;
            Show(Page::Settings);
            break;
        case Action::Done:
            Show(Page::Settings);
            break;
        case Action::Answer:
            if (Application::GetInstance().GetPhoneAiState() == PhoneAiState::Failed) {
                Application::GetInstance().RetryPhoneAi();
                break;
            }
            if (!gateway_manager_phone_command(GATEWAY_PHONE_CMD_ANSWER))
                Text(phone_state_, "接听请求未发送");
            else
                Text(phone_state_, "正在接听…");
            break;
        case Action::Reject:
            if (!gateway_manager_phone_command(GATEWAY_PHONE_CMD_REJECT))
                Text(phone_state_, "请求未发送，请重试");
            else
                Text(phone_state_, "等待电话确认…");
            break;
        case Action::Latest:
            following_ = true;
            scrolling_ = true;
            lv_obj_scroll_to_y(phone_transcripts_, LV_COORD_MAX, LV_ANIM_OFF);
            scrolling_ = false;
            Text(lv_obj_get_child(phone_pause_, 0), "暂停跟随");
            break;
        case Action::Pause:
            following_ = !following_;
            Text(lv_obj_get_child(phone_pause_, 0), following_ ? "暂停跟随" : "继续跟随");
            if (following_)
                Act(Action::Latest);
            break;
    }
    Refresh();
}
void PhoneSettingsView::AddTranscript(bool user, const char* text, const lv_font_t* font) {
    if (!text || !*text)
        return;
    // Keep at most 16 final turns, with a bounded UTF-8 message per turn.
    while (lv_obj_get_child_count(phone_transcripts_) >= 16)
        lv_obj_delete(lv_obj_get_child(phone_transcripts_, 0));
    size_t length = strnlen(text, 1024);
    while (length && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80)
        --length;
    std::string message = user ? "来电人：\n" : "AI：\n";
    message.append(text, length);
    auto label = Label(phone_transcripts_, message.c_str(), 0, 0, 274, user ? kWhite : kCyan);
    if (font)
        lv_obj_set_style_text_font(label, font, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_update_layout(phone_transcripts_);
    if (following_) {
        scrolling_ = true;
        lv_obj_scroll_to_y(phone_transcripts_, LV_COORD_MAX, LV_ANIM_OFF);
        scrolling_ = false;
    }
}

void PhoneSettingsView::RefreshPhone() {
    phone_status_t phone{};
    int64_t age = 0;
    uint32_t duration = 0;
    const bool fresh = gateway_manager_get_phone_status(&phone, &age, &duration);
    const auto ai = Application::GetInstance().GetPhoneAiState();
    if (fresh) {
        cached_phone_ = phone;
        cached_phone_at_ = lv_tick_get();
        cached_duration_ = duration;
    }
    const bool retained = !fresh && status_.ready && cached_phone_.phase == PHONE_ACTIVE &&
                          ai >= PhoneAiState::Connecting && ai <= PhoneAiState::Speaking &&
                          lv_tick_elaps(cached_phone_at_) < 5000;
    if (retained) {
        phone = cached_phone_;
        duration = cached_duration_ + lv_tick_elaps(cached_phone_at_);
    }
    const bool known = fresh || retained;
    const bool incoming = known && phone.phase == PHONE_INCOMING;
    const bool active = known && phone.phase == PHONE_ACTIVE;
    const bool call = incoming || active || (known && phone.phase == PHONE_OUTGOING);
    if (call && phone.call_id != phone_call_id_) {
        phone_call_id_ = phone.call_id;
        lv_obj_clean(phone_transcripts_);
        following_ = true;
    }
    if (call) {
        if (page_ == Page::Detail)
            CallRecords::Instance().StopPlayback();
        page_ = Page::Closed;
        Visible(root_, false);
        Visible(shade_, false);
    }
    Visible(entry_, false);
    Visible(phone_root_, page_ == Page::Closed || call);
    Visible(phone_nav_, !call);
    Visible(phone_transcripts_, active);
    Visible(phone_pause_, active);
    Visible(phone_latest_, active);
    Visible(phone_answer_, incoming || (active && ai == PhoneAiState::Failed));
    Text(lv_obj_get_child(phone_answer_, 0), active ? "重试 AI" : "AI 立即接听");
    Visible(phone_reject_, call);
    Text(lv_obj_get_child(phone_reject_, 0), incoming ? "拒接" : "挂断");
    Text(phone_title_, incoming ? "来电待接听"
                       : active ? "AI 通话中"
                       : call   ? "正在呼叫"
                                : "电话助手");
    Text(phone_number_, call ? (phone.caller_length ? phone.caller : "未知来电") : "待机");
    const char* ai_text = "AI 尚未配置";
    switch (ai) {
        case PhoneAiState::Ready:
            ai_text = "AI 已配置";
            break;
        case PhoneAiState::Connecting:
            ai_text = "AI 连接中";
            break;
        case PhoneAiState::Listening:
            ai_text = "AI 正在聆听";
            break;
        case PhoneAiState::Thinking:
            ai_text = "AI 正在思考";
            break;
        case PhoneAiState::Speaking:
            ai_text = "AI 正在回复";
            break;
        case PhoneAiState::Failed:
            switch (Application::GetInstance().GetPhoneAiFailure()) {
                case PhoneAiFailure::CloudConnect:
                    ai_text = "云端连接失败";
                    break;
                case PhoneAiFailure::CloudDisconnected:
                    ai_text = "云端连接中断";
                    break;
                case PhoneAiFailure::GatewayControl:
                    ai_text = "网关连接中断";
                    break;
                case PhoneAiFailure::GatewayMediaTimeout:
                    ai_text = "网关音频超时";
                    break;
                case PhoneAiFailure::GatewayMediaChanged:
                    ai_text = "网关音频已切换";
                    break;
                case PhoneAiFailure::AudioQueueOverflow:
                    ai_text = "音频缓存已满";
                    break;
                case PhoneAiFailure::AudioMemory:
                    ai_text = "音频内存不足";
                    break;
                case PhoneAiFailure::WorkerResources:
                    ai_text = "AI 任务资源不足";
                    break;
                default:
                    ai_text = "AI 故障，请重试";
                    break;
            }
            break;
        default:
            break;
    }
    Text(phone_state_, ai_text);
    Enable(phone_answer_,
           ai != PhoneAiState::Unavailable && (active || ai != PhoneAiState::Failed));
    char info[120];
    if (active) {
        snprintf(info, sizeof(info), "通话 %02lu:%02lu", (unsigned long)(duration / 60000),
                 (unsigned long)(duration / 1000 % 60));
    } else if (incoming && (phone.flags & PHONE_AUTO_PENDING)) {
        snprintf(info, sizeof(info), "%u 秒后自动接听", (phone.remaining_ms + 999) / 1000);
    } else if (incoming) {
        snprintf(info, sizeof(info), "自动接听已暂停");
    } else {
        time_t now = time(nullptr) + 8 * 3600;
        tm date{};
        gmtime_r(&now, &date);
        if (date.tm_year >= 124)
            strftime(info, sizeof(info), "%m/%d %H:%M", &date);
        else
            snprintf(info, sizeof(info), "时间未同步");
    }
    Text(phone_time_, info);
    snprintf(info, sizeof(info), "网关：%s\n手机：%s\nSCO：%s",
             status_.ready && status_.remote_fresh ? "在线" : "离线",
             status_.remote_fresh && status_.hfp == 3 ? "已连接" : "未连接",
             status_.remote_fresh && status_.sco == 3 ? "已连接" : "未连接");
    Text(phone_links_, info);
    Text(phone_notice_, active     ? "实时转写 · 录音状态检查中"
                        : incoming ? "接通后由 AI 对话\n可立即接听或拒接"
                        : call     ? "等待电话实际接通"
                        : !fresh   ? "等待电话网关连接"
                                   : "最近已接听电话");
    if (retained)
        Text(phone_notice_, "电话状态暂未更新 · 请检查连接");
}

void PhoneSettingsView::Refresh() {
    gateway_manager_get_ui_status(&status_);
    RefreshPhone();
    RefreshRecords();
    Text(gateway_, status_.bound ? status_.peer_id : "尚未绑定");
    Text(link_, !status_.available                      ? "双板连接：服务未启动"
                : !status_.wifi                         ? "双板连接：S3 未联网"
                : status_.ready && status_.remote_fresh ? "双板连接：已认证连接"
                : status_.bound                         ? "双板连接：等待恢复"
                                                        : "双板连接：未配置");
    Text(phone_, !status_.remote_fresh ? "手机 HFP：状态未知"
                 : status_.hfp == 3    ? "手机 HFP：已连接"
                                       : "手机 HFP：未连接");
    auto state = Application::GetInstance().GetDeviceState();
    Text(ai_, state == kDeviceStateListening || state == kDeviceStateSpeaking ? "AI 服务：会话中"
              : state == kDeviceStateConnecting                               ? "AI 服务：连接中"
                                                                              : "AI 服务：未连接");
    time_t now = time(nullptr);
    // The design's clock is China standard time. Do not change global TZ.
    now += 8 * 60 * 60;
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    char clock[40];
    if (timeinfo.tm_year >= 124)
        strftime(clock, sizeof(clock), "%m/%d %H:%M", &timeinfo);
    else
        snprintf(clock, sizeof(clock), "时间未同步");
    Text(clock_, clock);
    static bool sntp_started = false;
    if (status_.wifi && !sntp_started) {
        sntp_started = true;
        Application::GetInstance().Schedule([] {
            if (esp_sntp_enabled())
                return;
            esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
            esp_err_t err = esp_netif_sntp_init(&config);
            ESP_LOGI("phone_ui", "SNTP start=%s", esp_err_to_name(err));
        });
    }
    if (page_ == Page::Search) {
        bool queued = awaiting_scan_ && status_.scan_serial == scan_before_;
        if (!queued)
            awaiting_scan_ = false;
        bool scanning = queued || status_.scanning;
        char summary[120];
        if (scanning)
            snprintf(summary, sizeof(summary), "正在搜索附近设备…");
        else if (status_.error[0])
            snprintf(summary, sizeof(summary), "搜索失败：%s", status_.error);
        else if (status_.count)
            snprintf(summary, sizeof(summary), "发现 %u 个网关（完整设备 ID）", status_.count);
        else
            snprintf(summary, sizeof(summary), "未发现网关，请检查电源并重新搜索");
        Text(summary_, summary);
        for (unsigned i = 0; i < rows_.size(); ++i) {
            Visible(rows_[i], !queued && i < status_.count);
            if (i >= status_.count)
                continue;
            Text(row_ids_[i], status_.peers[i].id);
            char rssi[64];
            snprintf(rssi, sizeof(rssi), "信号 %d dBm%s", status_.peers[i].rssi,
                     !strcmp(status_.peer_id, status_.peers[i].id) ? " · 已绑定" : "");
            Text(row_rssi_[i], rssi);
            Enable(row_buttons_[i], !scanning);
        }
        Enable(retry_, !scanning);
        Enable(reprovision_, !scanning && status_.bound);
        if (queued && lv_tick_elaps(started_) > 3000) {
            awaiting_scan_ = false;
            Result("搜索未开始", Error(status_.error));
        }
    }
    if (page_ != Page::Provision || !operation_)
        return;
    if (status_.provisioning || status_.scanning ||
        (recovery_ && status_.scan_serial != scan_before_))
        accepted_ = true;
    const bool waiting = !strcmp(status_.stage, "waiting_button") && status_.provisioning;
    Visible(key_, waiting);
    Text(summary_, waiting               ? "等待实体按键确认"
                   : status_.config_sent ? "已发送配置，正在核验联网结果"
                                         : "连接与认证中，请稍候");
    Text(message_,
         waiting ? "请在经典 ESP32 网关上短按 Key1（BOOT）\n不要长按，也不要按 RESET"
         : recovery_
             ? "正在为已绑定网关恢复配网，无需再次按键。\n通过局域网认证握手后才算连接成功。"
             : "正在建立安全连接并配置 Wi-Fi。\n通过局域网认证握手后才算连接成功。");
    char countdown[80];
    if (waiting)
        snprintf(countdown, sizeof(countdown), "确认窗口约剩余 %lu 秒",
                 (unsigned long)((status_.remaining_ms + 999) / 1000));
    else
        snprintf(countdown, sizeof(countdown), "已等待 %lu 秒",
                 (unsigned long)(lv_tick_elaps(started_) / 1000));
    Text(countdown_, countdown);
    Enable(cancel_, !status_.config_sent);
    Enable(close_, !status_.config_sent);
    if (!accepted_ && lv_tick_elaps(started_) < 3000)
        return;
    if (accepted_ && !status_.provisioning && !status_.scanning && !status_.error[0]) {
        if (status_.ready && status_.remote_fresh && !strcmp(selected_, status_.peer_id)) {
            operation_ = false;
            Result("网关已连接",
                   "Wi-Fi 已配置，局域网认证握手通过。\n手机 HFP 状态可在设置页单独查看。");
        } else {
            if (!handshake_since_)
                handshake_since_ = lv_tick_get();
            Text(summary_, "Wi-Fi 配置结束，等待局域网认证");
            if (lv_tick_elaps(handshake_since_) > 30000) {
                operation_ = false;
                Result("等待认证超时",
                       "尚未收到新鲜认证状态，请检查两板是否在同一局域网。绑定状态以设置页为准。");
            }
        }
    } else if (!status_.provisioning && !status_.scanning && (!accepted_ || status_.error[0])) {
        operation_ = false;
        Result("配网未完成", Error(status_.error));
    }
}
bool PhoneSettingsView::Accessible(lv_obj_t* object) const {
    if (!lv_obj_is_visible(object))
        return false;
    const bool modal = page_ != Page::Closed && page_ != Page::Settings && page_ != Page::Records &&
                       page_ != Page::Detail;
    if (!modal)
        return true;
    // A modal also blocks the underlying settings controls for serial tests.
    for (auto parent = object; parent; parent = lv_obj_get_parent(parent))
        if (parent == dialog_)
            return true;
    return false;
}
bool PhoneSettingsView::Command(const char* command) {
    if (!strcmp(command, "ui transcripts")) {
        // Explicit diagnostic request only; no continuous transcript logging.
        const auto count = lv_obj_get_child_count(phone_transcripts_);
        for (unsigned i = 0; i < count; ++i) {
            auto row = cJSON_CreateObject();
            if (!row)
                return true;
            cJSON_AddNumberToObject(row, "index", i);
            cJSON_AddStringToObject(row, "text",
                                    lv_label_get_text(lv_obj_get_child(phone_transcripts_, i)));
            auto json = cJSON_PrintUnformatted(row);
            if (json) {
                printf("UI_TRANSCRIPT %s\n", json);
                cJSON_free(json);
            }
            cJSON_Delete(row);
        }
        printf("UI_TRANSCRIPTS_END count=%u\n", unsigned(count));
        return true;
    }
    if (!strcmp(command, "ui settings")) {
        Act(Action::Open);
        return true;
    }
    if (!strcmp(command, "ui home")) {
        if (!operation_)
            Act(Action::Home);
        return true;
    }
    if (!strcmp(command, "ui status")) {
        Refresh();
        printf(
            "UI_STATUS page=%u wifi=%d bound=%d ready=%d fresh=%d hfp=%u scan=%d count=%u "
            "operation=%d stage=%s error=%s lvgl_stack_free=%lu ai=%u transcripts=%u "
            "following=%d ai_error=%s\n",
            unsigned(page_), status_.wifi, status_.bound, status_.ready, status_.remote_fresh,
            status_.hfp, status_.scanning, status_.count, operation_, status_.stage, status_.error,
            (unsigned long)lvgl_stack_free_, unsigned(Application::GetInstance().GetPhoneAiState()),
            unsigned(lv_obj_get_child_count(phone_transcripts_)), following_,
            PhoneAiFailureName(Application::GetInstance().GetPhoneAiFailure()));
        return true;
    }
    if (!strcmp(command, "ui controls")) {
        lv_obj_update_layout(root_);
        lv_obj_update_layout(shade_);
        for (unsigned i = 0; i < target_count_; ++i) {
            const auto& t = targets_[i];
            if (!Accessible(t.object))
                continue;
            lv_area_t area;
            lv_obj_get_coords(t.object, &area);
            printf("UI_CONTROL action=%u index=%u x=%ld y=%ld width=%ld height=%ld enabled=%d\n",
                   unsigned(t.action), t.index, (long)area.x1, (long)area.y1,
                   (long)lv_area_get_width(&area), (long)lv_area_get_height(&area),
                   !lv_obj_has_state(t.object, LV_STATE_DISABLED));
        }
        return true;
    }
    unsigned seek = 0;
    int consumed = 0;
    if (sscanf(command, "ui seek %u%n", &seek, &consumed) == 1 && command[consumed] == 0 &&
        seek <= 1000 && page_ == Page::Detail && records_ && records_->playback_duration_ms) {
        lv_slider_set_value(playback_progress_, seek, LV_ANIM_OFF);
        lv_obj_send_event(playback_progress_, LV_EVENT_RELEASED, nullptr);
        return true;
    }
    consumed = 0;
    unsigned scroll = 0;
    if (sscanf(command, "ui scroll text %u%n", &scroll, &consumed) == 1 && command[consumed] == 0 &&
        scroll <= 100000 && page_ == Page::Detail) {
        lv_obj_scroll_to_y(lv_obj_get_parent(detail_text_), scroll, LV_ANIM_OFF);
        return true;
    }
    const char* action_text = command + (strncmp(command, "ui click ", 9) ? 0 : 9);
    struct Name {
        const char* name;
        Action action;
    };
    const Name names[] = {{"search", Action::Search},
                          {"rescan", Action::Rescan},
                          {"cancel", Action::Cancel},
                          {"close", Action::Close},
                          {"replace", Action::Replace},
                          {"reprovision", Action::Reprovision},
                          {"records", Action::Records},
                          {"rules", Action::Rules},
                          {"home", Action::Home},
                          {"answer", Action::Answer},
                          {"reject", Action::Reject},
                          {"hangup", Action::Reject},
                          {"latest", Action::Latest},
                          {"pause", Action::Pause},
                          {"records-next", Action::RecordsNext},
                          {"records-prev", Action::RecordsPrevious},
                          {"back", Action::DetailBack},
                          {"text-next", Action::TranscriptNext},
                          {"text-prev", Action::TranscriptPrevious},
                          {"play", Action::PlaybackToggle}};
    Action wanted = Action::Done;
    unsigned index = 0;
    bool matched = false;
    if (action_text != command) {
        for (const auto& name : names)
            if (!strcmp(name.name, action_text)) {
                wanted = name.action;
                matched = true;
            }
        int consumed = 0;
        if (sscanf(action_text, "connect %u%n", &index, &consumed) == 1 &&
            action_text[consumed] == 0) {
            wanted = Action::Select;
            matched = true;
        }
        consumed = 0;
        if (sscanf(action_text, "record %u%n", &index, &consumed) == 1 &&
            action_text[consumed] == 0) {
            wanted = Action::RecordSelect;
            matched = true;
        }
        consumed = 0;
        if (sscanf(action_text, "recent %u%n", &index, &consumed) == 1 &&
            action_text[consumed] == 0) {
            wanted = Action::RecentSelect;
            matched = true;
        }
    }
    if (!matched)
        return false;
    for (unsigned i = 0; i < target_count_; ++i) {
        auto& t = targets_[i];
        if (t.action == wanted && t.index == index && Accessible(t.object) &&
            !lv_obj_has_state(t.object, LV_STATE_DISABLED)) {
            lv_obj_send_event(t.object, LV_EVENT_CLICKED, nullptr);
            puts("UI_CLICK dispatched");
            return true;
        }
    }
    puts("UI_CLICK rejected_hidden_or_disabled");
    return true;
}
#endif  // CONFIG_GATEWAY_MANAGER
