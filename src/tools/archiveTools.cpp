/*
 * Copyright 2021 Maneesh P M <manu.pm55@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU  General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

#include "archiveTools.h"

#include "tools.h"
#include "pathTools.h"
#include "otherTools.h"
#include "stringTools.h"

#include <zim/error.h>
#include <zim/item.h>

namespace kiwix
{
std::string getMetadata(const zim::Archive& archive, const std::string& name) {
    try {
        return archive.getMetadata(name);
    } catch (zim::EntryNotFound& e) {
        return "";
    }
}

std::string getArchiveTitle(const zim::Archive& archive) {
  std::string value = getMetadata(archive, "Title");
  if (value.empty()) {
    value = getLastPathElement(archive.getFilename());
    std::replace(value.begin(), value.end(), '_', ' ');
    size_t pos = value.find(".zim");
    value = value.substr(0, pos);
  }
  return value;
}

std::string getMetaDescription(const zim::Archive& archive) {
  std::string value;
  value = getMetadata(archive, "Description");

  /* Mediawiki Collection tends to use the "Subtitle" name */
  if (value.empty()) {
    value = getMetadata(archive, "Subtitle");
  }

  return value;
}

std::string getMetaTags(const zim::Archive& archive, bool original) {
  std::string tags_str = getMetadata(archive, "Tags");
  if (original) {
    return tags_str;
  }
  auto tags = convertTags(tags_str);
  return join(tags, ";");
}

std::string getMetaLanguage(const zim::Archive& archive) {
  return getMetadata(archive, "Language");
}

std::string getMetaName(const zim::Archive& archive) {
  return getMetadata(archive, "Name");
}

std::string getMetaDate(const zim::Archive& archive) {
  return getMetadata(archive, "Date");
}

std::string getMetaCreator(const zim::Archive& archive) {
  return getMetadata(archive, "Creator");
}

std::string getMetaPublisher(const zim::Archive& archive) {
  return getMetadata(archive, "Publisher");
}

std::string getMetaFlavour(const zim::Archive& archive) {
  return getMetadata(archive, "Flavour");
}

std::string getArchiveId(const zim::Archive& archive) {
  return (std::string) archive.getUuid();
}

bool getArchiveFavicon(const zim::Archive& archive, unsigned size,
                           std::string& content, std::string& mimeType){
  try {
    auto item = archive.getIllustrationItem(size);
    content = item.getData();
    mimeType = item.getMimetype();
    return true;
  } catch(zim::EntryNotFound& e) {};

  return false;
}

// should this be in libzim
unsigned int getArchiveMediaCount(const zim::Archive& archive) {
  std::map<const std::string, unsigned int> counterMap = parseArchiveCounter(archive);
  unsigned int counter = 0;

  for (auto &pair:counterMap) {
    if (startsWith(pair.first, "image/") ||
        startsWith(pair.first, "video/") ||
        startsWith(pair.first, "audio/")) {
      counter += pair.second;
    }
  }

  return counter;
}

unsigned int getArchiveArticleCount(const zim::Archive& archive) {
  // [HACK]
  // getArticleCount() returns different things depending of the "version" of the zim.
  // On old zim (<=6), it returns the number of entry in `A` namespace
  // On recent zim (>=7), it returns:
  //  - the number of entry in `C` namespace (==getEntryCount) if no frontArticleIndex is present
  //  - the number of front article if a frontArticleIndex is present
  // The use case >=7 without frontArticleIndex is pretty rare so we don't care
  // We can detect if we are reading a zim <= 6 by checking if we have a newNamespaceScheme.
  if (archive.hasNewNamespaceScheme()) {
    //The articleCount is "good"
    return archive.getArticleCount();
  } else {
    // We have to parse the `M/Counter` metadata
    unsigned int counter = 0;
    for(const auto& pair:parseArchiveCounter(archive)) {
      if (startsWith(pair.first, "text/html")) {
        counter += pair.second;
      }
    }
    return counter;
  }
}

unsigned int getArchiveFileSize(const zim::Archive& archive) {
  return archive.getFilesize() / 1024;
}

zim::Item getFinalItem(const zim::Archive& archive, const zim::Entry& entry)
{
  return entry.getItem(true);
}

zim::Entry getEntryFromPath(const zim::Archive& archive, const std::string& path)
{
  try {
    return archive.getEntryByPath(path);
  } catch (zim::EntryNotFound& e) {
    if (path.empty() || path == "/") {
      return archive.getMainEntry();
    }
  }
  throw zim::EntryNotFound("Cannot find entry for non empty path");
}

MimeCounterType parseArchiveCounter(const zim::Archive& archive) {
  try {
    auto counterContent = archive.getMetadata("Counter");
    return parseMimetypeCounter(counterContent);
  } catch (zim::EntryNotFound& e) {
    return {};
  }
}

} // kiwix
