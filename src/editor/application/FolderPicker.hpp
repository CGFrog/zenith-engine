#pragma once

#include "editor/application/NativeFileDialog.hpp"

#include <filesystem>
#include <string>

// A "Browse..." button that fills a UTF-8 path text field from the operating
// system's folder dialog. The field stays typeable; the dialog is only a way
// to choose. The dialog is asynchronous, so draw() polls it every frame and
// applies the choice on the frame it arrives. A failed dialog is shown under
// the button instead of being dropped.
class FolderPicker {
  public:
    FolderPicker();

    // Draws the button (id must be unique among siblings). Returns true on
    // the frame a folder was written into path.
    bool draw(const char* id, std::string& path);

    // Applies a finished dialog to path; split out so tests can feed outcomes
    // without opening a window. Returns true when path changed.
    bool apply(const FileDialogOutcome& outcome, std::string& path);

    const std::string& error() const;

  private:
    NativeFileDialog m_dialog;
    std::string m_error;
};

// path as a setting value: relative to base when it lies inside base (so the
// project stays relocatable), otherwise unchanged. Purely lexical: it does not
// resolve symlinks or touch the file system.
std::filesystem::path relativeIfInside(const std::filesystem::path& path, const std::filesystem::path& base);
