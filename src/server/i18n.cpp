/*
 * Copyright 2022 Veloman Yunkan <veloman.yunkan@gmail.com>
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

#include "i18n.h"

#include <algorithm>
#include <map>

namespace kiwix
{

const char* I18nStringTable::get(const std::string& key) const
{
  const I18nString* const begin = entries;
  const I18nString* const end = begin + entryCount;
  const I18nString* found = std::lower_bound(begin, end, key,
      [](const I18nString& a, const std::string& k) {
        return a.key < k;
  });
  return found == end ? nullptr : found->value;
}

namespace i18n
{
// this data is generated by the i18n resource compiler
extern const I18nStringTable stringTables[];
extern const size_t langCount;
}

namespace
{

class I18nStringDB
{
public: // functions
  I18nStringDB() {
    for ( size_t i = 0; i < kiwix::i18n::langCount; ++i ) {
      const auto& t = kiwix::i18n::stringTables[i];
      lang2TableMap[t.lang] = &t;
    }
  };

  std::string get(const std::string& lang, const std::string& key) const {
    return getStringsFor(lang)->get(key);
  }

private: // functions
  const I18nStringTable* getStringsFor(const std::string& lang) const {
    try {
      return lang2TableMap.at(lang);
    } catch(const std::out_of_range&) {
      return lang2TableMap.at("en");
    }
  }

private: // data
  std::map<std::string, const I18nStringTable*> lang2TableMap;
};

} // unnamed namespace

std::string getTranslatedString(const std::string& lang, const std::string& key)
{
  static const I18nStringDB stringDb;

  return stringDb.get(lang, key);
}

} // namespace kiwix
