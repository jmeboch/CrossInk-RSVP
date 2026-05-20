#include "RsvpReaderActivity.h"

#include <Arduino.h>
#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <limits>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "EpubReaderChapterSelectionActivity.h"
#include "EpubReaderMenuActivity.h"
#include "EpubReaderPercentSelectionActivity.h"
#include "EpubReaderUtils.h"
#include "QrDisplayActivity.h"
#include "ReaderOptionsActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ScreenshotUtil.h"

namespace {
// 100% attempts roughly 240 WPM (250 ms/word). Slower settings scale the delay
// upward in 5% increments so hardware limits can be found empirically.
constexpr unsigned long BASE_WORD_DELAY_MS = 250UL;

int clampPercent(int percent) {
  if (percent < 0) return 0;
  if (percent > 100) return 100;
  return percent;
}

bool isBlankLine(const std::string& line) {
  return std::all_of(line.begin(), line.end(), [](unsigned char c) { return std::isspace(c) != 0; });
}

void pushWordsFromText(const std::string& text, std::vector<std::string>& out) {
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
      ++i;
    }
    const size_t start = i;
    while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) {
      ++i;
    }
    if (i > start) {
      out.emplace_back(text.substr(start, i - start));
    }
  }
}

void addParagraphStart(std::vector<size_t>& paragraphWordStarts, const size_t wordIndex) {
  if (paragraphWordStarts.empty() || paragraphWordStarts.back() != wordIndex) {
    paragraphWordStarts.push_back(wordIndex);
  }
}

void pushWordsFromPage(const Page& page, std::vector<std::string>& out, std::vector<size_t>& paragraphWordStarts,
                       const int expectedLineHeight) {
  bool havePreviousTextLine = false;
  int16_t previousTextLineY = 0;
  for (const auto& el : page.elements) {
    if (el->getTag() == TAG_PageLine) {
      const auto& line = static_cast<const PageLine&>(*el);
      if (!line.getBlock()) continue;
      const bool startsParagraph = out.empty() ||
                                   (havePreviousTextLine && line.yPos > previousTextLineY + expectedLineHeight + 2);
      const size_t startWordIndex = out.size();
      for (const auto& word : line.getBlock()->getWords()) {
        pushWordsFromText(word, out);
      }
      if (out.size() > startWordIndex) {
        if (startsParagraph) addParagraphStart(paragraphWordStarts, startWordIndex);
        previousTextLineY = line.yPos;
        havePreviousTextLine = true;
      }
    } else if (el->getTag() == TAG_PageTableFragment) {
      // Table text is not currently exposed by PageTableFragment; skip it rather than
      // forcing another EPUB parser in the hot path.
    }
  }
}
}  // namespace

void RsvpReaderActivity::onEnter() {
  Activity::onEnter();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  mappedInput.setReaderMode(true);
  pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  APP_STATE.openEpubPath = sourceType == SourceType::Epub ? epub->getPath() : txt->getPath();
  APP_STATE.saveToFile();

  if (sourceType == SourceType::Epub) {
    RECENT_BOOKS.addOrUpdateBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getThumbBmpPath());
    epub->setupCacheDir();
    loadEpubProgress();
  } else {
    const auto filePath = txt->getPath();
    const auto fileName = filePath.substr(filePath.rfind('/') + 1);
    RECENT_BOOKS.addOrUpdateBook(filePath, fileName, "", "");
    txt->setupCacheDir();
    buildTxtParagraphIndex();
    loadTxtProgress();
  }
  loadCurrentWords();
  requestUpdate();
}

void RsvpReaderActivity::onExit() {
  playing = false;
  if (sourceType == SourceType::Epub) {
    saveEpubProgress();
    section.reset();
  } else {
    saveTxtProgress();
  }
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();
  mappedInput.setReaderMode(false);
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  Activity::onExit();
}

void RsvpReaderActivity::loadEpubProgress() {
  FsFile f;
  if (Storage.openFileForRead("RSVP", epub->getCachePath() + "/rsvp_progress.bin", f)) {
    uint8_t data[4] = {0};
    if (f.read(data, sizeof(data)) == sizeof(data)) {
      currentSpineIndex = static_cast<int>((uint32_t)data[0] | ((uint32_t)data[1] << 8));
      currentParagraphIndex = static_cast<uint16_t>((uint32_t)data[2] | ((uint32_t)data[3] << 8));
      currentPage = 0;
      pendingEpubParagraphRestore = true;
      f.close();
      return;
    }
    f.close();
  }

  if (!Storage.openFileForRead("RSVP", epub->getCachePath() + "/progress.bin", f)) {
    return;
  }
  uint8_t data[6] = {0};
  const int n = f.read(data, sizeof(data));
  f.close();
  if (n >= 4) {
    currentSpineIndex = static_cast<int>((uint32_t)data[0] | ((uint32_t)data[1] << 8));
    currentPage = static_cast<int>((uint32_t)data[2] | ((uint32_t)data[3] << 8));
  }
}

void RsvpReaderActivity::saveEpubProgress() const {
  if (!epub) return;
  const int pageCount = section ? section->pageCount : 0;
  EpubReaderUtils::saveProgress(*epub, currentSpineIndex, currentPage, pageCount);

  FsFile f;
  if (!Storage.openFileForWrite("RSVP", epub->getCachePath() + "/rsvp_progress.bin", f)) return;
  const uint8_t data[4] = {static_cast<uint8_t>(currentSpineIndex & 0xFF),
                           static_cast<uint8_t>((currentSpineIndex >> 8) & 0xFF),
                           static_cast<uint8_t>(currentParagraphIndex & 0xFF),
                           static_cast<uint8_t>((currentParagraphIndex >> 8) & 0xFF)};
  f.write(data, sizeof(data));
  f.close();
}

bool RsvpReaderActivity::ensureEpubSection() {
  if (!epub) return false;
  const int spineCount = epub->getSpineItemsCount();
  if (spineCount <= 0) return false;
  currentSpineIndex = std::clamp(currentSpineIndex, 0, spineCount - 1);
  if (section) return true;

  const uint16_t viewportWidth = renderer.getScreenWidth() - (SETTINGS.screenMargin * 2);
  const uint16_t viewportHeight = renderer.getScreenHeight() - (SETTINGS.screenMargin * 2) -
                                  UITheme::getInstance().getStatusBarHeight();
  section = std::make_unique<Section>(epub, currentSpineIndex, renderer);
  const int fontId = SETTINGS.getReaderFontId();
  const float lineCompression = SETTINGS.getReaderLineCompression();
  const bool loaded = section->loadSectionFile(fontId, lineCompression, SETTINGS.extraParagraphSpacing,
                                               SETTINGS.forceParagraphIndents, SETTINGS.paragraphAlignment,
                                               viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
                                               SETTINGS.embeddedStyle, SETTINGS.imageRendering,
                                               SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled);
  if (!loaded && !section->createSectionFile(fontId, lineCompression, SETTINGS.extraParagraphSpacing,
                                             SETTINGS.forceParagraphIndents, SETTINGS.paragraphAlignment,
                                             viewportWidth, viewportHeight, SETTINGS.hyphenationEnabled,
                                             SETTINGS.embeddedStyle, SETTINGS.imageRendering,
                                             SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled)) {
    section.reset();
    return false;
  }
  if (section->pageCount == 0) return false;
  if (pendingEpubParagraphRestore) {
    if (const auto page = section->getPageForParagraphIndex(currentParagraphIndex)) {
      currentPage = *page;
    }
    pendingEpubParagraphRestore = false;
  }
  currentPage = std::clamp(currentPage, 0, static_cast<int>(section->pageCount) - 1);
  section->currentPage = currentPage;
  return true;
}

bool RsvpReaderActivity::loadEpubWords() {
  words.clear();
  paragraphWordStarts.clear();
  if (!ensureEpubSection()) return false;
  section->currentPage = currentPage;
  auto page = section->loadPageFromSectionFile();
  if (!page) return false;
  const int expectedLineHeight = static_cast<int>(renderer.getLineHeight(SETTINGS.getReaderFontId()) *
                                                 SETTINGS.getReaderLineCompression());
  pushWordsFromPage(*page, words, paragraphWordStarts, expectedLineHeight);
  if (const auto pIdx = section->getParagraphIndexForPage(static_cast<uint16_t>(currentPage))) {
    currentParagraphIndex = *pIdx;
  }
  return !words.empty();
}

bool RsvpReaderActivity::moveEpubPage(bool forward) {
  if (!ensureEpubSection()) return false;
  if (forward) {
    if (currentPage + 1 < section->pageCount) {
      ++currentPage;
      return true;
    }
    if (currentSpineIndex + 1 < epub->getSpineItemsCount()) {
      ++currentSpineIndex;
      currentPage = 0;
      section.reset();
      return true;
    }
    return false;
  }
  if (currentPage > 0) {
    --currentPage;
    return true;
  }
  if (currentSpineIndex > 0) {
    --currentSpineIndex;
    currentPage = 0;
    section.reset();
    if (ensureEpubSection()) {
      currentPage = section->pageCount > 0 ? section->pageCount - 1 : 0;
    }
    return true;
  }
  return false;
}

void RsvpReaderActivity::previousEpubParagraph() {
  playing = false;
  if (previousInCurrentWordList()) {
    saveEpubProgress();
    requestUpdate();
    return;
  }
  if (!ensureEpubSection()) return;
  if (!moveEpubPage(false)) {
    currentWordIndex = 0;
    saveEpubProgress();
    requestUpdate();
    return;
  }
  loadCurrentWords();
  if (!paragraphWordStarts.empty()) {
    currentWordIndex = paragraphWordStarts.back();
  }
  saveEpubProgress();
  requestUpdate();
}

bool RsvpReaderActivity::previousInCurrentWordList() {
  if (words.empty() || paragraphWordStarts.empty()) return false;

  size_t target = 0;
  bool found = false;
  for (const size_t start : paragraphWordStarts) {
    if (start >= currentWordIndex) break;
    target = start;
    found = true;
  }

  if (!found && currentWordIndex > 0) {
    target = 0;
    found = true;
  }

  if (found) {
    currentWordIndex = target;
  }
  return found;
}

float RsvpReaderActivity::getBookProgressPercent() const {
  if (!epub || !section || section->pageCount == 0) return 0.0f;
  const float chapterProgress = static_cast<float>(currentPage) / static_cast<float>(section->pageCount);
  return epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
}

void RsvpReaderActivity::applyMenuOrientation(const uint8_t orientation) {
  if (SETTINGS.orientation == orientation) return;

  SETTINGS.orientation = orientation;
  SETTINGS.saveToFile();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  if (sourceType == SourceType::Epub) {
    section.reset();
    loadCurrentWords();
  }
  requestUpdate();
}

void RsvpReaderActivity::jumpToPercent(int percent) {
  if (sourceType != SourceType::Epub || !epub || epub->getBookSize() == 0 || epub->getSpineItemsCount() == 0) return;

  percent = clampPercent(percent);
  size_t targetSize = (epub->getBookSize() / 100) * static_cast<size_t>(percent) +
                      (epub->getBookSize() % 100) * static_cast<size_t>(percent) / 100;
  if (percent >= 100) {
    targetSize = epub->getBookSize() - 1;
  }

  int targetSpineIndex = epub->getSpineItemsCount() - 1;
  size_t prevCumulative = 0;
  for (int i = 0; i < epub->getSpineItemsCount(); i++) {
    const size_t cumulative = epub->getCumulativeSpineItemSize(i);
    if (targetSize <= cumulative) {
      targetSpineIndex = i;
      prevCumulative = i > 0 ? epub->getCumulativeSpineItemSize(i - 1) : 0;
      break;
    }
  }

  const size_t cumulative = epub->getCumulativeSpineItemSize(targetSpineIndex);
  const size_t spineSize = cumulative > prevCumulative ? cumulative - prevCumulative : 0;
  const float spineProgress = spineSize == 0 ? 0.0f : static_cast<float>(targetSize - prevCumulative) / spineSize;

  playing = false;
  currentSpineIndex = targetSpineIndex;
  currentPage = 0;
  currentParagraphIndex = 0;
  section.reset();
  if (ensureEpubSection() && section->pageCount > 0) {
    currentPage = static_cast<int>(spineProgress * static_cast<float>(section->pageCount));
    if (currentPage >= section->pageCount) currentPage = section->pageCount - 1;
    if (const auto pIdx = section->getParagraphIndexForPage(static_cast<uint16_t>(currentPage))) {
      currentParagraphIndex = *pIdx;
    }
  }
  loadCurrentWords();
  saveEpubProgress();
  requestUpdate();
}

void RsvpReaderActivity::buildTxtParagraphIndex() {
  txtParagraphOffsets.clear();
  FsFile f;
  if (!Storage.openFileForRead("RSVP", txt->getPath(), f)) return;
  std::string line;
  size_t lineStart = 0;
  while (f.available()) {
    lineStart = f.position();
    line.clear();
    while (f.available()) {
      const char c = static_cast<char>(f.read());
      if (c == '\n') break;
      if (c != '\r') line.push_back(c);
    }
    if (isBlankLine(line)) {
      continue;
    } else {
      txtParagraphOffsets.push_back(lineStart);
    }
  }
  f.close();
  if (txtParagraphOffsets.empty()) txtParagraphOffsets.push_back(0);
}

void RsvpReaderActivity::loadTxtProgress() {
  FsFile f;
  if (!Storage.openFileForRead("RSVP", txt->getCachePath() + "/rsvp_progress.bin", f)) return;
  uint8_t data[4] = {0};
  if (f.read(data, sizeof(data)) == sizeof(data)) {
    const uint32_t saved = (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
                           ((uint32_t)data[3] << 24);
    txtCurrentParagraph = std::min<size_t>(saved, txtParagraphOffsets.empty() ? 0 : txtParagraphOffsets.size() - 1);
  }
  f.close();
}

void RsvpReaderActivity::saveTxtProgress() const {
  FsFile f;
  if (!txt || !Storage.openFileForWrite("RSVP", txt->getCachePath() + "/rsvp_progress.bin", f)) return;
  const uint32_t value = static_cast<uint32_t>(txtCurrentParagraph);
  const uint8_t data[4] = {static_cast<uint8_t>(value & 0xFF), static_cast<uint8_t>((value >> 8) & 0xFF),
                           static_cast<uint8_t>((value >> 16) & 0xFF), static_cast<uint8_t>((value >> 24) & 0xFF)};
  f.write(data, sizeof(data));
  f.close();
}

bool RsvpReaderActivity::loadTxtWords() {
  words.clear();
  paragraphWordStarts.clear();
  if (txtParagraphOffsets.empty()) return false;
  txtCurrentParagraph = std::min(txtCurrentParagraph, txtParagraphOffsets.size() - 1);
  FsFile f;
  if (!Storage.openFileForRead("RSVP", txt->getPath(), f)) return false;
  if (!f.seek(txtParagraphOffsets[txtCurrentParagraph])) {
    f.close();
    return false;
  }
  std::string paragraph;
  while (f.available()) {
    const char c = static_cast<char>(f.read());
    if (c == '\n') break;
    if (c != '\r') paragraph.push_back(c);
  }
  f.close();
  paragraphWordStarts.push_back(0);
  pushWordsFromText(paragraph, words);
  return !words.empty();
}

bool RsvpReaderActivity::moveTxtParagraph(bool forward) {
  if (txtParagraphOffsets.empty()) return false;
  if (forward) {
    if (txtCurrentParagraph + 1 >= txtParagraphOffsets.size()) return false;
    ++txtCurrentParagraph;
  } else {
    if (txtCurrentParagraph == 0) return false;
    --txtCurrentParagraph;
  }
  return true;
}

void RsvpReaderActivity::loadCurrentWords() {
  currentWordIndex = 0;
  atEnd = false;
  bool ok = false;
  while (true) {
    ok = sourceType == SourceType::Epub ? loadEpubWords() : loadTxtWords();
    if (ok) break;
    if (sourceType == SourceType::Epub) {
      if (!moveEpubPage(true)) break;
    } else {
      if (!moveTxtParagraph(true)) break;
    }
  }
  if (!ok) {
    words.clear();
    paragraphWordStarts.clear();
    atEnd = true;
    playing = false;
  }
}

unsigned long RsvpReaderActivity::wordDelayMs() const {
  const uint8_t pct = std::clamp<uint8_t>(SETTINGS.rsvpSpeedPercent, 5, 100);
  unsigned long delayMs = (BASE_WORD_DELAY_MS * 100UL) / pct;
  if (!words.empty() && currentWordIndex < words.size()) {
    const size_t wordsAtATime = std::clamp<uint8_t>(SETTINGS.rsvpWordsAtATime, 1, 3);
    const size_t lastWordIndex = std::min(words.size() - 1, currentWordIndex + wordsAtATime - 1);
    const char last = words[lastWordIndex].empty() ? '\0' : words[lastWordIndex].back();
    if (last == '.' || last == '!' || last == '?') delayMs += delayMs / 2;
    if (last == ',' || last == ';' || last == ':') delayMs += delayMs / 4;
  }
  return delayMs;
}

void RsvpReaderActivity::advanceWord() {
  if (atEnd) return;
  const size_t wordsAtATime = std::clamp<uint8_t>(SETTINGS.rsvpWordsAtATime, 1, 3);
  if (currentWordIndex + wordsAtATime < words.size()) {
    currentWordIndex += wordsAtATime;
  } else {
    const bool moved = sourceType == SourceType::Epub ? moveEpubPage(true) : moveTxtParagraph(true);
    if (!moved) {
      atEnd = true;
      playing = false;
      return;
    }
    loadCurrentWords();
  }
  if (sourceType == SourceType::Epub) {
    saveEpubProgress();
  } else {
    saveTxtProgress();
  }
  requestUpdate();
}

void RsvpReaderActivity::openReaderOptions() {
  playing = false;
  startActivityForResult(std::make_unique<ReaderOptionsActivity>(renderer, mappedInput, true), [this](const ActivityResult&) {
    SETTINGS.saveToFile();
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    if (sourceType == SourceType::Epub) {
      section.reset();
      loadCurrentWords();
    }
    requestUpdate();
  });
}

void RsvpReaderActivity::openReaderMenu() {
  playing = false;
  if (sourceType != SourceType::Epub || !epub) {
    openReaderOptions();
    return;
  }

  ensureEpubSection();
  const int pageCount = section ? section->pageCount : 0;
  const int pageNumber = pageCount > 0 ? currentPage + 1 : 0;
  const int bookProgressPercent = clampPercent(static_cast<int>(getBookProgressPercent() + 0.5f));
  startActivityForResult(
      std::make_unique<EpubReaderMenuActivity>(renderer, mappedInput, epub->getTitle(), pageNumber, pageCount,
                                               bookProgressPercent, SETTINGS.orientation, false, false, false, false,
                                               false, 0, true),
      [this](const ActivityResult& result) {
        const auto& menu = std::get<MenuResult>(result.data);
        applyMenuOrientation(menu.orientation);
        if (menu.settingsChanged && sourceType == SourceType::Epub) {
          SETTINGS.saveToFile();
          section.reset();
          loadCurrentWords();
          requestUpdate();
        }
        if (!result.isCancelled) {
          onReaderMenuConfirm(static_cast<EpubReaderMenuActivity::MenuAction>(menu.action));
        }
      });
}

void RsvpReaderActivity::onReaderMenuConfirm(const EpubReaderMenuActivity::MenuAction action) {
  if (sourceType != SourceType::Epub || !epub) return;

  switch (action) {
    case EpubReaderMenuActivity::MenuAction::SELECT_CHAPTER: {
      startActivityForResult(
          std::make_unique<EpubReaderChapterSelectionActivity>(renderer, mappedInput, epub, epub->getPath(),
                                                               currentSpineIndex),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              playing = false;
              currentSpineIndex = std::get<ChapterResult>(result.data).spineIndex;
              currentPage = 0;
              currentParagraphIndex = 0;
              section.reset();
              loadCurrentWords();
              saveEpubProgress();
            }
            requestUpdate();
          });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_TO_PERCENT: {
      const int initialPercent = clampPercent(static_cast<int>(getBookProgressPercent() + 0.5f));
      startActivityForResult(std::make_unique<EpubReaderPercentSelectionActivity>(renderer, mappedInput, initialPercent),
                             [this](const ActivityResult& result) {
                               if (!result.isCancelled) {
                                 jumpToPercent(std::get<PercentResult>(result.data).percent);
                               }
                               requestUpdate();
                             });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::DISPLAY_QR: {
      startActivityForResult(std::make_unique<QrDisplayActivity>(renderer, mappedInput, currentDisplayText()),
                             [this](const ActivityResult&) { requestUpdate(); });
      break;
    }
    case EpubReaderMenuActivity::MenuAction::GO_HOME:
      onGoHome();
      break;
    case EpubReaderMenuActivity::MenuAction::DELETE_CACHE:
      saveEpubProgress();
      section.reset();
      epub->clearCache();
      epub->setupCacheDir();
      onGoHome();
      break;
    case EpubReaderMenuActivity::MenuAction::SCREENSHOT:
      pendingScreenshot = true;
      requestUpdate();
      break;
    case EpubReaderMenuActivity::MenuAction::READER_OPTIONS:
    case EpubReaderMenuActivity::MenuAction::CONTROLS_OPTIONS:
    case EpubReaderMenuActivity::MenuAction::ROTATE_SCREEN:
      // Handled inside EpubReaderMenuActivity or by its result callback.
      requestUpdate();
      break;
    case EpubReaderMenuActivity::MenuAction::AUTO_PAGE_TURN:
    case EpubReaderMenuActivity::MenuAction::FOOTNOTES:
    case EpubReaderMenuActivity::MenuAction::SYNC:
    case EpubReaderMenuActivity::MenuAction::READING_STATS:
    case EpubReaderMenuActivity::MenuAction::TOGGLE_COMPLETED:
    case EpubReaderMenuActivity::MenuAction::BOOKMARK_TOGGLE:
    case EpubReaderMenuActivity::MenuAction::VIEW_BOOKMARKS:
    case EpubReaderMenuActivity::MenuAction::DELETE_BOOKMARKS:
      requestUpdate();
      break;
  }
}

void RsvpReaderActivity::loop() {
  const bool backPressed = mappedInput.isPressed(MappedInputManager::Button::Back);
  if (!backPressed) {
    backButtonHandled = false;
  } else if (!backButtonHandled) {
    backButtonHandled = true;
    finish();
    return;
  }

  const bool confirmPressed = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  if (!confirmPressed) {
    confirmButtonHandled = false;
  } else if (!confirmButtonHandled) {
    confirmButtonHandled = true;
    openReaderMenu();
    return;
  }

  const bool playPausePressed = mappedInput.isPressed(MappedInputManager::Button::PageForward) ||
                                mappedInput.isPressed(MappedInputManager::Button::Right);
  if (!playPausePressed) {
    playPauseButtonHandled = false;
  } else if (!playPauseButtonHandled) {
    playPauseButtonHandled = true;
    playing = !playing;
    lastWordMs = millis();
    requestUpdate();
    return;
  }

  const bool previousPressed = mappedInput.isPressed(MappedInputManager::Button::PageBack) ||
                               mappedInput.isPressed(MappedInputManager::Button::Left);
  if (!previousPressed) {
    previousButtonHandled = false;
  } else if (!previousButtonHandled) {
    previousButtonHandled = true;
    if (sourceType == SourceType::Epub) {
      previousEpubParagraph();
    } else {
      playing = false;
      if (!previousInCurrentWordList()) {
        moveTxtParagraph(false);
        loadCurrentWords();
      }
      saveTxtProgress();
      requestUpdate();
    }
    return;
  }

  if (playing && RenderLock::peek()) {
    // The render task owns the display/state while the current word is still
    // being pushed to e-paper. Do not advance underneath it; that can race the
    // render and leave controls looking unresponsive at high RSVP speeds.
    lastWordMs = millis();
    return;
  }

  if (playing && millis() - lastWordMs >= wordDelayMs()) {
    lastWordMs = millis();
    advanceWord();
  }
}

std::string RsvpReaderActivity::currentDisplayText() const {
  if (words.empty() || currentWordIndex >= words.size()) return "";
  const size_t wordsAtATime = std::clamp<uint8_t>(SETTINGS.rsvpWordsAtATime, 1, 3);
  const size_t end = std::min(words.size(), currentWordIndex + wordsAtATime);
  std::string text;
  for (size_t i = currentWordIndex; i < end; ++i) {
    if (!text.empty()) text.push_back(' ');
    text += words[i];
  }
  return text;
}

void RsvpReaderActivity::renderChrome() const {
  char status[64];
  snprintf(status, sizeof(status), "%s  %u%%", playing ? tr(STR_PLAYING) : tr(STR_PAUSED),
           static_cast<unsigned>(SETTINGS.rsvpSpeedPercent));

  float progress = 0.0f;
  int position = 1;
  int total = 1;
  if (sourceType == SourceType::Epub) {
    if (epub && section && section->pageCount > 0) {
      const float chapterProgress = static_cast<float>(currentPage) / static_cast<float>(section->pageCount);
      progress = epub->calculateProgress(currentSpineIndex, chapterProgress) * 100.0f;
      position = currentPage + 1;
      total = section->pageCount;
    }
  } else if (!txtParagraphOffsets.empty()) {
    progress = (static_cast<float>(txtCurrentParagraph) / static_cast<float>(txtParagraphOffsets.size())) * 100.0f;
    position = static_cast<int>(txtCurrentParagraph) + 1;
    total = static_cast<int>(txtParagraphOffsets.size());
  }

  GUI.drawStatusBar(renderer, progress, position, total, status);
}

void RsvpReaderActivity::render(RenderLock&&) {
  renderer.clearScreen();
  if (atEnd) {
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_END_OF_BOOK), true,
                              EpdFontFamily::BOLD);
  } else if (words.empty()) {
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_NO_TEXT), true,
                              EpdFontFamily::BOLD);
  } else {
    const std::string text = currentDisplayText();
    renderer.drawCenteredText(SETTINGS.getReaderFontId(), renderer.getScreenHeight() / 2, text.c_str(), true,
                              EpdFontFamily::BOLD);
  }
  renderChrome();
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
  if (pendingScreenshot) {
    pendingScreenshot = false;
    ScreenshotUtil::takeScreenshot(renderer);
  }
  if (playing) {
    // Start the next-word timer after the blocking e-paper update completes.
    // Otherwise playback can saturate the render task and short hardware taps are missed.
    lastWordMs = millis();
  }
}
