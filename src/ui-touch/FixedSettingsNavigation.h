// SPDX-License-Identifier: GPL-3.0-or-later
// Included by UITask.cpp after the settings builders, on CAP_FIXED_NAVIGATION boards only.
struct FixedSettingsPage {
  lv_coord_t top;
  lv_coord_t height;
};

struct FixedSettingsPager {
  lv_obj_t* view = nullptr;
  lv_obj_t* footer = nullptr;
  lv_obj_t* previous = nullptr;
  lv_obj_t* next = nullptr;
  lv_obj_t* count = nullptr;
  lv_coord_t height = 0;
  size_t index = 0;
  std::vector<FixedSettingsPage> pages;
};

static lv_obj_t* fixedSettingsButton(lv_obj_t* parent, const char* title, lv_event_cb_t callback, void* data) {
  lv_obj_t* button = lv_btn_create(parent);
  lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
  lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_PANEL), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_CONTROL), LV_STATE_PRESSED);
  lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_CONTROL), LV_STATE_FOCUS_KEY);
  lv_obj_set_style_border_width(button, s_theme_high_contrast ? 2 : 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(button, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
  lv_obj_set_style_radius(button, SC(8), LV_PART_MAIN);
  fixedMenuProtectTap(button);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, data);
  lv_obj_t* label = lv_label_create(button);
  lv_label_set_text(label, title);
  lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
  lv_obj_set_style_text_font(label, &g_font_semi_14, LV_PART_MAIN);
  lv_obj_center(label);
  return button;
}

static void fixedSettingsPagerMove(FixedSettingsPager& pager) {
  const auto& page = pager.pages[pager.index];
  lv_obj_set_height(pager.view, page.height + lv_obj_get_style_pad_top(pager.view, LV_PART_MAIN));
  lv_obj_update_layout(pager.view);
  lv_obj_scroll_to_y(pager.view, page.top, LV_ANIM_OFF);
  char count[32];
  snprintf(count, sizeof count, "%u/%u", (unsigned)pager.index + 1, (unsigned)pager.pages.size());
  lv_label_set_text(pager.count, count);
  fixedPagerButtons(pager.previous, pager.next, pager.index > 0, pager.index + 1 < pager.pages.size());
}

static void fixedSettingsPageClick(lv_event_t* event) {
  auto& pager = *static_cast<FixedSettingsPager*>(lv_event_get_user_data(event));
  if (pager.pages.size() < 2) return;
  if (lv_event_get_current_target(event) == pager.previous && pager.index > 0) --pager.index;
  else if (lv_event_get_current_target(event) == pager.next && pager.index + 1 < pager.pages.size()) ++pager.index;
  fixedSettingsPagerMove(pager);
}

static void fixedSettingsPagerInit(FixedSettingsPager& pager, lv_obj_t* view) {
  pager = {};
  pager.view = view;
  lv_obj_update_layout(view);
  pager.height = lv_obj_get_height(view);
  lv_obj_clear_flag(view, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(view, LV_SCROLLBAR_MODE_OFF);
  pager.footer = setBox(lv_obj_get_parent(view));
  lv_obj_set_size(pager.footer, lv_pct(100), SC(36));
  lv_obj_align(pager.footer, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(pager.footer, LV_OBJ_FLAG_HIDDEN);
  pager.previous = fixedSettingsButton(pager.footer, TR("Previous"), fixedSettingsPageClick, &pager);
  pager.next = fixedSettingsButton(pager.footer, TR("Next"), fixedSettingsPageClick, &pager);
  for (lv_obj_t* button : {pager.previous, pager.next}) {
    lv_obj_set_size(button, lv_pct(40), SC(32));
    lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);
    lv_obj_t* label = lv_obj_get_child(button, 0);
    lv_obj_set_style_text_font(label, uiChromeFont(), LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(label, lv_pct(100));
  }
  lv_obj_align(pager.previous, LV_ALIGN_LEFT_MID, 4, 0);
  lv_obj_align(pager.next, LV_ALIGN_RIGHT_MID, -4, 0);
  pager.count = lv_label_create(pager.footer);
  lv_obj_set_style_text_font(pager.count, &g_font_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(pager.count, lv_color_hex(COLOR_TEXT), LV_PART_MAIN);
  lv_obj_center(pager.count);
}

static void fixedSettingsRowOffsets(lv_obj_t* parent, lv_coord_t origin, lv_coord_t height,
                                    std::vector<FixedMenuRow>& rows) {
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(parent); ++i) {
    lv_obj_t* row = lv_obj_get_child(parent, i);
    if (lv_obj_has_flag(row, LV_OBJ_FLAG_HIDDEN) || lv_obj_has_flag(row, LV_OBJ_FLAG_FLOATING)) continue;
    lv_area_t bounds;
    lv_obj_get_coords(row, &bounds);
    if (lv_obj_get_height(row) > height && lv_obj_get_child_cnt(row))
      fixedSettingsRowOffsets(row, origin, height, rows);
    else
      rows.push_back({bounds.y1 - origin, bounds.y2 + 1 - origin, 0, 0});
  }
}

static void fixedSettingsPagerFit(FixedSettingsPager& pager) {
  lv_obj_t* view = pager.view;
  lv_obj_set_height(view, pager.height);
  lv_obj_update_layout(view);

  if (lv_obj_get_style_flex_flow(view, LV_PART_MAIN) == LV_FLEX_FLOW_ROW_WRAP) {
    lv_coord_t tile_height = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(view); ++i) {
      auto* tile = lv_obj_get_child(view, i);
      if (lv_obj_check_type(tile, &lv_btn_class) && !lv_obj_has_flag(tile, LV_OBJ_FLAG_HIDDEN))
        tile_height = std::max(tile_height, lv_obj_get_height(tile));
    }
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(view); ++i) {
      auto* tile = lv_obj_get_child(view, i);
      if (lv_obj_check_type(tile, &lv_btn_class) && !lv_obj_has_flag(tile, LV_OBJ_FLAG_HIDDEN))
        lv_obj_set_height(tile, tile_height);
    }
    lv_obj_update_layout(view);
  }

  pager.pages.clear();
  if (lv_obj_get_scroll_y(view) + lv_obj_get_scroll_bottom(view) <= 0) {
    lv_obj_add_flag(pager.footer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_y(view, 0, LV_ANIM_OFF);
    pager.index = 0;
    return;
  }

  const lv_coord_t height = pager.height - lv_obj_get_style_height(pager.footer, LV_PART_MAIN);
  const lv_coord_t pad_top = lv_obj_get_style_pad_top(view, LV_PART_MAIN);
  const int room = std::max(1, (int)height - pad_top);
  lv_area_t bounds;
  lv_obj_get_content_coords(view, &bounds);
  std::vector<FixedMenuRow> rows;
  fixedSettingsRowOffsets(view, bounds.y1 - lv_obj_get_scroll_y(view), room, rows);
  std::stable_sort(rows.begin(), rows.end(), [](const FixedMenuRow& a, const FixedMenuRow& b) { return a.top < b.top; });
  packFixedMenuRows(rows.data(), rows.size(), 0, room);

  // End each page after its final row so the start of the next one cannot be tapped.
  for (size_t first = 0; first < rows.size();) {
    int top = std::max(0, rows[first].top - rows[first].y);
    int bottom = rows[first].bottom;
    size_t last = first + 1;
    for (; last < rows.size() && rows[last].page == rows[first].page; ++last)
      bottom = std::max(bottom, rows[last].bottom);
    while (bottom - top > room) {
      pager.pages.push_back({(lv_coord_t)top, (lv_coord_t)room});
      top += std::max(1, room - SC(12));  // overlap long text that cannot fit on a page
    }
    pager.pages.push_back({(lv_coord_t)top, (lv_coord_t)std::max(1, bottom - top)});
    first = last;
  }
  if (pager.pages.size() < 2) {
    lv_obj_add_flag(pager.footer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_y(view, 0, LV_ANIM_OFF);
    pager.index = 0;
    return;
  }
  lv_obj_clear_flag(pager.footer, LV_OBJ_FLAG_HIDDEN);
  pager.index = std::min(pager.index, pager.pages.size() - 1);
  fixedSettingsPagerMove(pager);
}

static lv_obj_t* fixedSettingsTile(lv_obj_t* parent, const char* icon, const char* title, uint32_t hue,
                                   lv_event_cb_t callback, void* data, lv_obj_t** summary = nullptr,
                                   bool full_width = false) {
  lv_obj_update_layout(parent);
  const int available_width = lv_obj_get_content_width(parent);
  const int columns = available_width < 255 && lv_font_get_line_height(&g_font_semi_12) > 19 ? 1 : 2;
  const int gap = lv_obj_get_style_pad_column(parent, LV_PART_MAIN);
  const int width = full_width ? available_width : (available_width - gap * (columns - 1)) / columns;
  constexpr int inset = 6;
  const int text_width = std::max(1, width - 2 * inset);
  const int caption_height = summary ? lv_font_get_line_height(&g_font_12) : 0;

  const int title_len = strlen(title);
  const int spacing = !strchr(title, ' ') &&
      lv_txt_get_width(title, title_len, &g_font_semi_12, 0, LV_TEXT_FLAG_NONE) > text_width &&
      lv_txt_get_width(title, title_len, &g_font_semi_12, -1, LV_TEXT_FLAG_NONE) <= text_width ? -1 : 0;
  lv_point_t text_size;
  lv_txt_get_size(&text_size, title, &g_font_semi_12, spacing, 0, text_width, LV_TEXT_FLAG_NONE);
  const int height = std::max(44, (int)text_size.y + caption_height + (summary ? 3 : 0) + 10);
  lv_obj_t* button = fixedSettingsButton(parent, title, callback, data);
  lv_obj_set_size(button, width, height);
  lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);
  lv_obj_t* label = lv_obj_get_child(button, 0);
  lv_obj_set_size(label, text_width, LV_SIZE_CONTENT);
  lv_obj_set_style_text_font(label, &g_font_semi_12, LV_PART_MAIN);
  if (spacing) lv_obj_set_style_text_letter_space(label, spacing, LV_PART_MAIN);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  lv_obj_align(label, summary ? LV_ALIGN_TOP_LEFT : LV_ALIGN_LEFT_MID, inset, summary ? 5 : 0);

  if (icon && icon[0]) {
    const lv_font_t* font = isUiIconGlyph(icon) ? &ui_icons_16 : uiChromeFont();
    const int glyph_width = lv_txt_get_width(icon, strlen(icon), font, 0, LV_TEXT_FLAG_NONE);
    if (text_size.y == lv_font_get_line_height(&g_font_semi_12) && text_size.x + glyph_width + 4 <= text_width) {
      lv_obj_t* glyph = lv_label_create(button);
      lv_label_set_text(glyph, icon);
      lv_obj_set_style_text_font(glyph, font, LV_PART_MAIN);
      lv_obj_set_style_text_color(glyph, lv_color_hex(hue), LV_PART_MAIN);
      lv_obj_align(glyph, summary ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_RIGHT_MID, -inset, summary ? 4 : 0);
    }
  }
  if (summary) {
    *summary = lv_label_create(button);
    lv_label_set_long_mode(*summary, LV_LABEL_LONG_DOT);
    lv_obj_set_size(*summary, text_width, caption_height);
    lv_obj_set_style_text_font(*summary, &g_font_12, LV_PART_MAIN);
    if (caption_height > 15) lv_obj_set_style_text_letter_space(*summary, -1, LV_PART_MAIN);
    lv_obj_set_style_text_color(*summary, lv_color_hex(COLOR_SUB), LV_PART_MAIN);
    lv_obj_align(*summary, LV_ALIGN_BOTTOM_LEFT, inset, -5);
  }
  return button;
}

static void fixedSettingsGrid(lv_obj_t* view) {
  lv_obj_set_flex_flow(view, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(view, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_all(view, 7, LV_PART_MAIN);
  lv_obj_set_style_pad_row(view, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_column(view, 6, LV_PART_MAIN);
}

struct FixedSettingsSection {
  FixedSettingsSection(lv_obj_t* block, const char* heading) : content(block), title(heading) {}

  lv_obj_t* content;
  std::string title;
  lv_obj_t* slot = nullptr;
  lv_obj_t* tile = nullptr;
  size_t group = 0;
};

static std::vector<FixedSettingsSection> s_fixed_sections;
static FixedSettingsPager s_fixed_section_pager, s_fixed_detail_pager, s_fixed_landing_pager;

#if CAP_KEYPAD_NAV
static bool fixedSettingsOutsidePage(lv_obj_t* control) {
  if (!fixedNavigationEnabled()) return false;
  for (lv_obj_t* parent = lv_obj_get_parent(control); parent; parent = lv_obj_get_parent(parent)) {
    for (const auto* pager : {&s_fixed_section_pager, &s_fixed_detail_pager, &s_fixed_landing_pager}) {
      if (parent != pager->view || !lv_obj_is_valid(pager->footer) || lv_obj_has_flag(pager->footer, LV_OBJ_FLAG_HIDDEN))
        continue;
      lv_area_t viewport, bounds;
      lv_obj_get_coords(parent, &viewport);
      lv_obj_get_coords(control, &bounds);
      return bounds.y1 < viewport.y1 || bounds.y2 > viewport.y2;
    }
  }
  return false;
}
#endif

static lv_obj_t* s_fixed_section_view = nullptr;
static size_t s_fixed_open_group = 0;
static bool s_fixed_settings_building = false;

static void fixedSettingsRememberBlock(lv_obj_t* parent, lv_obj_t* block, const char* title) {
  if (s_fixed_settings_building && parent == s_settings_page)
    s_fixed_sections.push_back({block, title ? title : ""});
}

static void fixedSettingsDetailClear() {
  s_fixed_sections.clear();
  s_fixed_section_pager = {};
  s_fixed_detail_pager = {};
  s_fixed_section_view = nullptr;
  s_fixed_open_group = 0;
}

// Look inside layout containers. A card heading and its panel are decoration;
// visible help, status text and controls (including disabled ones) are content.
// Only inspect the block itself: the pager hides its slot when showing the index.
static bool fixedSettingsHasContent(lv_obj_t* obj) {
  if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return false;
  if (lv_obj_check_type(obj, &lv_label_class))
    return lv_obj_get_user_data(obj) != &s_settings_card_heading_tag && lv_label_get_text(obj)[0];
  if (!lv_obj_check_type(obj, &lv_obj_class)) return true;
  const uint32_t children = lv_obj_get_child_cnt(obj);
  // Colour swatches and custom-drawn readouts can be bare LVGL objects. Their
  // handlers provide their interaction or drawing even when they have no label.
  if (!children) return obj->spec_attr && obj->spec_attr->event_dsc_cnt;
  for (uint32_t i = 0; i < children; ++i)
    if (fixedSettingsHasContent(lv_obj_get_child(obj, i))) return true;
  return false;
}

static void fixedSettingsSetVisible(lv_obj_t* obj, bool visible) {
  if (visible) lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void fixedSettingsDetailShow(size_t group);

static void fixedSettingsDetailRefit() {
  bool open_group_available = false;
  for (const auto& section : s_fixed_sections) {
    if (!section.slot) continue;
    const bool available = fixedSettingsHasContent(section.content);
    if (section.tile) fixedSettingsSetVisible(section.tile, available);
    // Untitled blocks stay inline. Named blocks are reached through their tiles.
    const bool inline_block = !section.tile;
    fixedSettingsSetVisible(section.slot, available && (inline_block || section.group == s_fixed_open_group));
    if (section.tile && section.group == s_fixed_open_group) open_group_available = available;
  }
  if (s_fixed_open_group && !open_group_available) {
    fixedSettingsDetailShow(0);
    return;
  }
  fixedSettingsPagerFit(s_fixed_section_view && !s_fixed_open_group ? s_fixed_section_pager : s_fixed_detail_pager);
}

static void fixedSettingsDetailShow(size_t group) {
  // Save the active field before hiding either its section or an inline index block.
  const bool changing = group != s_fixed_open_group;
  s_fixed_open_group = group;
  if (changing && s_kb_bind_ta && lv_obj_is_valid(s_kb_bind_ta)) {
    lv_event_send(s_kb_bind_ta, LV_EVENT_DEFOCUSED, nullptr);
    hideKb();
  }
  fixedSettingsSetVisible(s_fixed_section_view, !group);
  fixedSettingsSetVisible(s_fixed_detail_pager.view, group);
  if (group) {
    lv_obj_add_flag(s_fixed_section_pager.footer, LV_OBJ_FLAG_HIDDEN);
    s_fixed_detail_pager.index = 0;
    lv_obj_scroll_to_y(s_fixed_detail_pager.view, 0, LV_ANIM_OFF);
  } else {
    lv_obj_add_flag(s_fixed_detail_pager.footer, LV_OBJ_FLAG_HIDDEN);
  }
  fixedSettingsDetailRefit();
  navMarkDirty();
  updateGlobalStatusBar();
}

static void fixedSettingsSectionClick(lv_event_t* event) {
  fixedSettingsDetailShow(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
}

static void fixedSettingsDetailInit(lv_obj_t* page) {
  fixedSettingsPagerInit(s_fixed_detail_pager, page);
  std::vector<FixedSettingsSection> ordered;
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(page); ++i) {
    lv_obj_t* child = lv_obj_get_child(page, i);
    if (lv_obj_has_flag(child, LV_OBJ_FLAG_FLOATING)) continue;
    auto block = std::find_if(s_fixed_sections.begin(), s_fixed_sections.end(),
                              [child](const FixedSettingsSection& section) { return section.content == child; });
    if (block == s_fixed_sections.end()) ordered.push_back({child, ""});
    else ordered.push_back(std::move(*block));
  }
  s_fixed_sections = std::move(ordered);
  // Keep presentation visibility on a separate slot. Builders can still hide or
  // reveal the real card independently, including while its section is closed.
  for (auto& section : s_fixed_sections) {
    section.slot = setBox(page);
    lv_obj_set_size(section.slot, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(section.slot, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_parent(section.content, section.slot);
    fixedSettingsSetVisible(section.slot, fixedSettingsHasContent(section.content));
  }

  lv_obj_update_layout(page);
  if (lv_obj_get_scroll_bottom(page) <= 0 || s_fixed_sections.size() < 2 ||
      std::none_of(s_fixed_sections.begin(), s_fixed_sections.end(),
                   [](const FixedSettingsSection& section) { return !section.title.empty(); })) {
    fixedSettingsDetailRefit();
    return;
  }

  s_fixed_section_view = setBox(lv_obj_get_parent(page));
  lv_obj_set_size(s_fixed_section_view, lv_obj_get_width(page), s_fixed_detail_pager.height);
  lv_obj_set_pos(s_fixed_section_view, lv_obj_get_x(page), lv_obj_get_y(page));
  fixedSettingsGrid(s_fixed_section_view);
  lv_obj_set_style_pad_top(s_fixed_section_view, lv_obj_get_style_pad_top(page, LV_PART_MAIN), LV_PART_MAIN);
  fixedSettingsPagerInit(s_fixed_section_pager, s_fixed_section_view);
  for (size_t i = 0; i < s_fixed_sections.size(); ++i) {
    auto& section = s_fixed_sections[i];
    if (section.title.empty()) {
      lv_obj_set_parent(section.slot, s_fixed_section_view);
      continue;
    }
    // The status bar already names the open section. Its single card needs
    // neither a repeated heading nor the spacing reserved for the next card.
    auto* heading = lv_obj_get_child(section.content, 0);
    if (heading && lv_obj_get_user_data(heading) == &s_settings_card_heading_tag) {
      lv_obj_add_flag(heading, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_pad_bottom(section.content, 0, LV_PART_MAIN);
    }
    section.group = i + 1;
    section.tile = fixedSettingsTile(s_fixed_section_view, nullptr, section.title.c_str(), COLOR_TEXT,
                                     fixedSettingsSectionClick, reinterpret_cast<void*>(section.group));
  }
  fixedSettingsDetailShow(0);
}

static bool fixedSettingsDetailBack() {
  if (!s_fixed_open_group) return false;
  fixedSettingsDetailShow(0);
  return true;
}

static const char* fixedSettingsDetailTitle() {
  for (const auto& section : s_fixed_sections)
    if (s_fixed_open_group && section.group == s_fixed_open_group)
      return section.title.c_str();
  return nullptr;
}
