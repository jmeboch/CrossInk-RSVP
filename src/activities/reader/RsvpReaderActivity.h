#pragma once

#include <Epub.h>
#include <Epub/Section.h>
#include <Txt.h>

#include <memory>
#include <string>
#include <vector>

#include "EpubReaderMenuActivity.h"
#include "activities/Activity.h"

class RsvpReaderActivity final : public Activity {
  enum class SourceType : uint8_t { Epub, Txt };

  SourceType sourceType;
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Txt> txt;

  std::unique_ptr<Section> section;
  int currentSpineIndex = 0;
  int currentPage = 0;
  uint16_t currentParagraphIndex = 0;
  bool pendingEpubParagraphRestore = false;

  std::vector<size_t> txtParagraphOffsets;
  size_t txtCurrentParagraph = 0;

  std::vector<std::string> words;
  std::vector<size_t> paragraphWordStarts;
  size_t currentWordIndex = 0;
  bool playing = false;
  bool atEnd = false;
  bool pendingScreenshot = false;
  unsigned long lastWordMs = 0;
  int pagesUntilFullRefresh = 0;
  bool backButtonHandled = false;
  bool confirmButtonHandled = false;
  bool playPauseButtonHandled = false;
  bool previousButtonHandled = false;

  void loadEpubProgress();
  void saveEpubProgress() const;
  bool ensureEpubSection();
  bool loadEpubWords();
  bool moveEpubPage(bool forward);
  void previousEpubParagraph();
  bool previousInCurrentWordList();
  float getBookProgressPercent() const;
  void applyMenuOrientation(uint8_t orientation);
  void jumpToPercent(int percent);

  void buildTxtParagraphIndex();
  void loadTxtProgress();
  void saveTxtProgress() const;
  bool loadTxtWords();
  bool moveTxtParagraph(bool forward);

  void loadCurrentWords();
  void advanceWord();
  unsigned long wordDelayMs() const;
  void openReaderOptions();
  void openReaderMenu();
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  std::string currentDisplayText() const;
  void renderChrome() const;

 public:
  explicit RsvpReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("RsvpReader", renderer, mappedInput), sourceType(SourceType::Epub), epub(std::move(epub)) {}
  explicit RsvpReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Txt> txt)
      : Activity("RsvpReader", renderer, mappedInput), sourceType(SourceType::Txt), txt(std::move(txt)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return playing; }
  bool preventAutoSleep() override { return playing; }
  bool isReaderActivity() const override { return true; }
  bool canSnapshotForSleepOverlay() const override { return false; }
};
