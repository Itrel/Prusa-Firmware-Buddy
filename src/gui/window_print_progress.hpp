// window_progress.hpp

#pragma once

#include "window_numb.hpp"
#include "window_progress.hpp"

// to be used in ctor

class WindowPrintProgress : public window_numberless_progress_t {
    int8_t last_sd_percent_done;

public:
    WindowPrintProgress(window_t *parent, Rect16 rect);

protected:
    void windowEvent(window_t *sender, GUI_event_t event, void *param) override;
};

class WindowNumbPrintProgress : public window_numb_t {
    int8_t last_sd_percent_done;
    bool percent_changed;

public:
    WindowNumbPrintProgress(window_t *parent, Rect16 rect);
    int8_t getPercentage();

protected:
    void windowEvent(window_t *sender, GUI_event_t event, void *param) override;
};

class WindowPrintVerticalProgress : public window_vertical_progress_t {
    int8_t last_sd_percent_done;

public:
    WindowPrintVerticalProgress(window_t *parent, Rect16 rect);

protected:
    void windowEvent(window_t *sender, GUI_event_t event, void *param) override;
};
