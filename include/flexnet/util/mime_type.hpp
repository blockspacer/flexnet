#pragma once

#include <boost/algorithm/string.hpp>

#include <base/macros.h>
#include <base/sequence_checker.h>

#include <functional>
#include <map>
#include <string>

namespace flexnet {

// Used to detect Media Type (formerly known as MIME type)
// based on file suffix.
// See for details:
// https://www.iana.org/assignments/media-types/media-types.xhtml
class MimeType
{
public:
  struct IgnoreCaseMimeTypeComparator
    : std::binary_function<std::string, std::string, bool>
  {
    bool operator()
      (const std::string & s1, const std::string & s2) const
    {
      return boost::algorithm::lexicographical_compare(
        s1, s2, boost::algorithm::is_iless());
    }
  };

  using MimeTypesMap
    = std::map<std::string, std::string,
    IgnoreCaseMimeTypeComparator>;

public:
  MimeType();

  ~MimeType();

  const std::string& operator()
    (const std::string& file_suffix) const;

private:
  MimeTypesMap mime_types_;

  // fallback to "application/octet-stream"
  // if mime type can not be detected
  const std::string default_mime_type_;

  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(MimeType);
};

} // namespace flexnet
