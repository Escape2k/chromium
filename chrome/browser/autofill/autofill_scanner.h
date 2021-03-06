// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_AUTOFILL_AUTOFILL_SCANNER_H_
#define CHROME_BROWSER_AUTOFILL_AUTOFILL_SCANNER_H_
#pragma once

#include <vector>

#include "base/basictypes.h"
#include "base/string16.h"

class AutofillField;

// A helper class for parsing a stream of |AutofillField|'s with lookahead.
class AutofillScanner {
 public:
  explicit AutofillScanner(const std::vector<const AutofillField*>& fields);
  ~AutofillScanner();

  // Advances the cursor by one step, if possible.
  void Advance();

  // Returns the current field in the stream, or |NULL| if there are no more
  // fields in the stream.
  const AutofillField* Cursor() const;

  // Returns |true| if the cursor has reached the end of the stream.
  bool IsEnd() const;

  // Restores the most recently saved cursor. See also |SaveCursor()|.
  void Rewind();

  // Repositions the cursor to the specified |index|. See also |SaveCursor()|.
  void RewindTo(size_t index);

  // Saves and returns the current cursor position. See also |Rewind()| and
  // |RewindTo()|.
  size_t SaveCursor();

 private:
  // Indicates the current position in the stream, represented as a vector.
  std::vector<const AutofillField*>::const_iterator cursor_;

  // The most recently saved cursor.
  std::vector<const AutofillField*>::const_iterator saved_cursor_;

  // The beginning pointer for the stream.
  const std::vector<const AutofillField*>::const_iterator begin_;

  // The past-the-end pointer for the stream.
  const std::vector<const AutofillField*>::const_iterator end_;

  DISALLOW_COPY_AND_ASSIGN(AutofillScanner);
};

#endif  // CHROME_BROWSER_AUTOFILL_AUTOFILL_SCANNER_H_
