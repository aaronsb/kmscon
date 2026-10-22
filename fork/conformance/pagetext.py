#!/usr/bin/env python3
"""Text for the conformance pages.

SPDX-License-Identifier: MIT
"""

import re


def prose(text):
    """`text` as a markdown paragraph that shows it literally: a caption is
    written for a person reading the screen, so its asterisks, underscores,
    backticks, brackets and angle brackets are characters, not markup."""
    return re.sub(r"([\\`*_\[\]<>#~])", r"\\\1", text)
