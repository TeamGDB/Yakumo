#pragma once

namespace mhp3rd::ui {

[[nodiscard]] bool transmog_page_open();
[[nodiscard]] bool transmog_subpage_open();
bool transmog_page(bool back, bool menu_paused);
void reset_transmog_page();
[[nodiscard]] bool take_transmog_close_request();

} // namespace mhp3rd::ui
