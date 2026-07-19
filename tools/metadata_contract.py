#!/usr/bin/env python3
"""Shared release-metadata validation helpers for repository tools."""

from __future__ import annotations

import re


_RFC3339_RE = re.compile(
    r"(?P<year>[0-9]{4})-(?P<month>[0-9]{2})-(?P<day>[0-9]{2})"
    r"T(?P<hour>[0-9]{2}):(?P<minute>[0-9]{2}):(?P<second>[0-9]{2})"
    r"(?:\.(?P<fraction>[0-9]{1,9}))?"
    r"(?P<zone>Z|(?P<sign>[+-])(?P<offset_hour>[0-9]{2}):(?P<offset_minute>[0-9]{2}))",
    re.ASCII,
)


def _is_leap_year(year: int) -> bool:
    return year % 4 == 0 and (year % 100 != 0 or year % 400 == 0)


def is_rfc3339_timestamp(value: str) -> bool:
    """Return whether value uses the updater's strict RFC 3339 profile."""

    match = _RFC3339_RE.fullmatch(value)
    if match is None:
        return False
    year = int(match.group("year"))
    month = int(match.group("month"))
    day = int(match.group("day"))
    hour = int(match.group("hour"))
    minute = int(match.group("minute"))
    second = int(match.group("second"))
    if year == 0 or month < 1 or month > 12 or hour > 23 or minute > 59 or second > 59:
        return False
    days = (31, 29 if _is_leap_year(year) else 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31)
    if day < 1 or day > days[month - 1]:
        return False
    if match.group("zone") != "Z":
        offset_hour = int(match.group("offset_hour"))
        offset_minute = int(match.group("offset_minute"))
        if offset_hour > 23 or offset_minute > 59:
            return False
        if match.group("sign") == "-" and offset_hour == 0 and offset_minute == 0:
            return False
    return True
