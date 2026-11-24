#ifndef EVO_SUBTITLE_PICKER_SCREEN_HPP
#define EVO_SUBTITLE_PICKER_SCREEN_HPP

#include "evo/screens/StatefulScreen.hpp"
#include <string>
#include <vector>

namespace evo {

class SubtitlePickerScreen : public StatefulScreen {
public:
    SubtitlePickerScreen();
    ~SubtitlePickerScreen() override = default;

    ScreenId getScreenId() const override { return ScreenId::SubtitlePicker; }
    void onEnter() override;
    void onExit() override;
    bool handleInput(uint32_t pressed, uint32_t held, uint32_t released) override;
    void update(double deltaMs) override;
    void render(uint32_t* framebuffer, int width, int height) override;

private:
    struct SubtitleTrackEntry {
        int trackId = -2; // -2 = Off, -1 = External SRT, -3 = AUTO-SYNC, >= 0 = Stream index,
                          // <= kOnlineBase = an online subtitle not loaded yet,
                          // kExtSidecarBase - i = a sidecar .srt next to the movie
        std::string label;
        std::string detail;
        std::string extPath;      // sidecar rows: the file this row stands for
        bool isSecondary = false; // #110: this track is the second line of dialogue
    };

    /* subtitles.rml has six rows; scrolling by any other count lets the focus
     * walk onto rows that are never drawn. */
    static constexpr int kVisibleRows = 6;
    static constexpr int kAutoSyncTrack = -3;
    static constexpr int kLoadFileTrack = -4;   /* opens SubtitleFileScreen */
    /* Online subtitle i (not loaded yet) is trackId kOnlineBase - i. */
    static constexpr int kOnlineBase = -100;
    static constexpr int kMaxOnlineRows = 24;
    /* Web-subtitle rows (Podnapisi / OpenSubtitles / SubDL search,
     * docs/planning/websubs.md): result i is kWebSubBase - i; the
     * SEARCH ONLINE... action row opens the keyboard. */
    static constexpr int kWebSubBase = -300;
    static constexpr int kWebSearchTrack = -301;
    static constexpr int kMaxWebRows = 20;
    /* Sidecar rows: when the movie has several external .srt files next to
     * it, one row each (kExtSidecarBase - i) instead of a single EXTERNAL
     * row, so the user picks which one is active. */
    static constexpr int kExtSidecarBase = -50;
    static constexpr int kMaxExtSidecars = 8;

    void refreshTracks();
    void navigate(int delta);
    void activateSelection();
    void activateSecondary();
    void cycleSize();
    void cycleSecondarySize();
    void rollRandomCues();
    static void OnWebSearchSubmitted(const char *text, void *userdata);

    std::vector<SubtitleTrackEntry> m_tracks;
    int m_selectedIndex = 0;
    int m_activeTrackIndex = 0;
    int m_scrollOffset = 0;
    /* the web search finishes on its own thread; repaint the rows when its
     * status changes (docs/planning/websubs.md) */
    int m_lastWebStatus = -1;
    /* hold R2 for ~3 s to enter DELETE MODE (unload a loaded subtitle,
     * deleting the file EVO downloaded for it) */
    bool m_deleteMode = false;
    bool m_r2Held = false;
    int m_r2HoldMs = 0;
    /* delete mode replaces the track list with everything EVO downloaded
     * for this movie, shown in red */
    struct DeleteRow {
        std::string path;
        std::string name;
    };
    std::vector<DeleteRow> m_delList;
    void rebuildDeleteList();
    void deleteFocusedSubtitle();
    /* the external-subtitle state as the picker last drew it: when these
     * change under us (a download landed), the rows repaint so the new
     * subtitle shows as the selected track */
    int m_seenExtCount = -1;
    int m_seenExtActive = -1;
    char m_seenExtLabel[96] = {0};
    /* >0 when the movie has several sidecar .srt files (rows replace the
     * plain EXTERNAL row) */
    int m_extSidecarCount = 0;

    std::string m_previewPrimary;
    std::string m_previewSecondary;
};

} // namespace evo

#endif // EVO_SUBTITLE_PICKER_SCREEN_HPP
