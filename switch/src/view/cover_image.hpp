#pragma once

#include <borealis.hpp>

#include "util/async.hpp"

/**
 * Immagine che si scarica da sola dal sito della fonte (JPEG, PNG o WebP).
 * Mantiene una piccola cache in memoria dei byte scaricati.
 */
class CoverImage : public brls::Image {
  public:
    CoverImage();
    ~CoverImage() override;

    void setUrl(const std::string& pathOrUrl);

    static void clearCache();

  private:
    void setBytes(const std::string& data);
    AliveToken alive = makeAlive();
    std::string url;
};
