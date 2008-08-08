#!/usr/bin/env python
# $Id: $
# $Change: $
# $DateTime: $
# $Author: $
"""
Toolkit for filtering input from GPDB server logs

Module contents:

    ---- High level filters
    FilterLogEntries() - the one-stop filter shop

    ---- Identity filters which observe a stream without altering it
    TimestampSpy() - count lines and note timestamp range

    ---- Grouping and ungrouping filters
    GroupByTimestamp() - group lines with same timestamp
    Ungroup() - decompose groups into lines
    EnumerateUngroup() - number the groups and decompose into lines

    ---- Selection filters
    MatchRegex() - select lines or groups in which a regular expr is matched
    NoMatchRegex() - select lines or groups in which a regular expr has no match
    MatchInFirstLine() - select groups in which regex has a match in first line
    NoMatchInFirstLine() - select groups in which regex doesn't match in first line

    ---- Utilities for setting up filter parameters
    filterize() - wrap a filter for use in FilterLogEntries filter list
    spiffInterval() - get begin/end datetime given any subset of begin/end/duration
"""

from datetime import date, datetime, time, timedelta
import re
import time

# This pattern matches the date and time stamp at the beginning of a line
# in a GPDB log file.  The timestamp format is: YYYY-MM-DD HH:MM:SS[.frac]
# A timezone specifier may follow the timestamp, but we ignore that.
timestampPattern = re.compile(r'\d\d\d\d-\d\d-\d\d \d\d:\d\d:\d\d(\.\d*)?')


def FilterLogEntries(iterable,
                     msgfile=None,
                     begin=None,
                     end=None,
                     include=None,
                     exclude=None,
                     filters=[]):
    """
    Generator to consume the lines of a GPDB log file from iterable,
    yield the lines which satisfy the given criteria, and skip the rest.

    iterable should be a sequence of strings, an already-open input file,
    or some object which supports iteration and yields strings.

    msgfile should be an already-open output file to which status messages
    are to be written; or None.

    For our purposes, a log entry consists of a line which starts with a
    timestamp in YYYY-MM-DD HH:MM:SS[.fraction] format, followed by zero
    or more lines having the same timestamp or no timestamp.

    begin should be a datetime.datetime or datetime.date object, or None.
    Log entries are skipped if their timestamp is less than the specified
    date and time.  Fractional seconds and timezones are ignored.  If a
    date object is given, it is converted to a datetime with time 00:00:00.

    end is like begin, except that it causes log entries to be skipped if
    their timestamp is greater than or equal to the specified date and time.

    include should be a regular expression object returned by the re.compile()
    method; or a string specifying a regular expression according to the rules
    of the re package in the Python standard library; or a list of such objects
    and/or strings; or None.  A log entry is skipped if there is an include
    regex which - in every line of the entry - fails to match.

    exclude is like include, except that it causes a log entry to be skipped
    if there is an exclude regex which matches in some line of the entry.

    filters is a sequence of callables.  Each callable will be called just
    once, with one argument: an input stream, which will be an iterator
    yielding groups.  (Here a 'group' is a sequence of strings: the lines
    of a log entry.)  The callable should return an iterator yielding
    filtered groups.  The filters are applied in the order given,
    downstream of the begin/end/include/exclude filters.  For example,
    this filter selects log entries with 'DEBUGn:' in the first line...
        lambda(iterable): MatchInFirstLine(iterable, r'DEBUG\d:')

    Regular expression syntax is at http://docs.python.org/lib/re-syntax.html

    At the beginning of a log file before the first timestamped line there
    could be some lines with no timestamp.  If begin or end is not None, any
    such lines are skipped.  Otherwise they are grouped together and treated
    as one log entry.
    """
    iterable = iter(iterable)
    spyMid = None

    if msgfile:
        iterable = spyIn = TimestampSpy(iterable)

    # Select log entries such that begin <= timestamp < end
    if begin or end:
        iterable = TimestampInBounds(iterable, begin, end)
        if msgfile:
            iterable = spyMid = TimestampSpy(iterable)

    if include or exclude or filters:
        # We want patterns to be tested entry-by-entry rather than line-by-line,
        # so group together the lines of each entry.
        iterable = GroupByTimestamp(iterable)

        # Include matching log entries.
        if (isinstance(include, basestring) or
            hasattr(include, 'search')):     # one string or compiled regex
            iterable = MatchRegex(iterable, include)
        elif include:                        # a list: all must match
            for regex in include:
                iterable = MatchRegex(iterable, regex)

        # Exclude non-matching log entries.
        if (isinstance(exclude, basestring) or
            hasattr(exclude, 'search')):     # one string or compiled regex
            iterable = NoMatchRegex(iterable, exclude)
        elif exclude:                        # a list: all must fail to match
            for regex in exclude:
                iterable = NoMatchRegex(iterable, regex)

        # Apply filters.
        for filter in filters:
            iterable = filter(iterable)

        # Break the groups back down into lines for output.
        iterable = Ungroup(iterable)

    if msgfile:
        iterable = spyOut = TimestampSpy(iterable)

    for line in iterable:
        yield line

    if msgfile:
        str_rangeIn = spyIn.str_range()
        str_rangeMid = (spyMid and spyMid.str_range()) or ''
        str_rangeOut = spyOut.str_range()

        # Unfiltered input statistics
        if str_rangeIn:
            print >>msgfile, ('%7d lines in;  timestamps from %s to %s'
                              % (spyIn.lines, str_rangeIn[0], str_rangeIn[1]))
        else:
            print >>msgfile, ('%7d lines in;  no timestamps found'
                              % spyIn.lines)

        # Log entries where begin <= timestamp < end, before any other filtering
        if str_rangeMid:
            print >>msgfile, ('%7d in range;  timestamps from %s to %s'
                              % (spyMid.lines, str_rangeMid[0], str_rangeMid[1]))
        elif begin or end:
            print >>msgfile, '%7d in range' % spyMid.lines

        # Filtered output statistics
        if str_rangeOut:
            print >>msgfile, ('%7d lines out; timestamps from %s to %s'
                              % (spyOut.lines, str_rangeOut[0], str_rangeOut[1]))
        else:
            print >>msgfile, '%7d lines out' % spyOut.lines


class TimestampSpy(object):
    """
    Iterator to pass through a stream of GPDB log entries while noting the
    lowest and highest timestamps.

    TimestampSpy(iterable) -> iterator
        iterable -- a sequence, iterator, file, or other object which
            supports iteration.  Each item returned by its next() method
            must be either a string (a line of GPDB log data) or a group
            (a sequence of strings where the timestamp, if any, is at the
            beginning of the first string).
    """
    def __init__(self, iterable):
        self.source = iter(iterable)
        self.minstamp = '\xff'
        self.maxstamp = ''
        self.items = 0
        self.lines = 0
        self.groups = 0

    def __iter__(self):
        return self

    def next(self):
        item = self.source.next()
        self.items += 1

        if isinstance(item, basestring):     # ungrouped input
            s = item                         # item is a string
            self.lines += 1
        elif len(item) > 0:                  # grouped input
            s = item[0]                      # item is a sequence of strings
            self.lines += len(item)
            self.groups += 1
        else:                                # item is an empty sequence
            s = ''
            self.groups += 1

        if self.minstamp > s:
            m = timestampPattern.match(s)
            if m:
                self.minstamp = m.group(0)
        if self.maxstamp < s:
            m = timestampPattern.match(s)
            if m:
                self.maxstamp = m.group(0)
        return item

    def str_range(self):
        if self.maxstamp == '':
            return None
        return (self.minstamp, self.maxstamp)

    def datetime_range(self):
        if self.maxstamp == '':
            return None
        minstruct = time.strptime(self.minstamp, '%Y-%m-%d %H:%M:%S')[:6]
        maxstruct = time.strptime(self.maxstamp, '%Y-%m-%d %H:%M:%S')[:6]
        return (datetime(*minstruct), datetime(*maxstruct))

#-------------------------------------------------------------------------

def GroupByTimestamp(iterable):
    """
    Generator to consume lines of a GPDB log and group them into lists.
    A new group is started whenever the timestamp changes.

    The first call to next() yields a (possibly empty) list of all lines
    found before the first timestamped line.  Subsequently, each call
    yields a list of one or more lines, in which the first line starts
    with a timestamp, and the rest have the same timestamp or no timestamp.

    The inverse of GroupByTimestamp is Ungroup.

    GroupByTimestamp(iterable) -> iterator
        iterable -- a sequence, iterator, file, or other object which
            supports iteration.  Each item returned by its next() method
            must be a string.

    Example:
        # Print log entries from stdin having 'ERROR:' in the first line.
        from logfilter import GroupByTimestamp
        from sys import stdin
        for lines in GroupByTimestamp(stdin):
            if lines[0].find('ERROR:'):
                for line in lines:
                    print line
    """
    source = iter(iterable)

    # Build list of lines preceding the first timestamped line, and yield
    # it as the first group (could be empty).
    lines = []
    while True:
        try:
            s = source.next()
        except StopIteration:
            yield lines
            return
        tsmatch = timestampPattern.match(s)
        if tsmatch:
            break
        lines.append(s)

    while True:
        # Previous group is finished... output it
        yield lines

        # Start a new group.  Save timestamp from its first line.
        lines = [s]
        timestamp = tsmatch.group(0)
        tsmatch = None

        # Any more lines with same (or no) timestamp?  Add them to the list.
        while True:
            try:
                s = source.next()
            except StopIteration:            # end of data
                yield lines                  # output the final group
                return
            if not s.startswith(timestamp):
                tsmatch = timestampPattern.match(s)
                if tsmatch:                  # line has a different timestamp
                    break
            lines.append(s)


def Ungroup(iterable):
    """
    Generator which takes a stream of groups and flattens it by one level,
    yielding the elements of the groups.  Here, the term 'group' means
    a sequence, iterator, or some object which supports iteration.

    Ungroup(iterable) -> iterator
        iterable -- a sequence, iterator, or some object which supports
            iteration.  Each item returned by its next() method must be
            a sequence, iterator, or iterable object.

    Example:
        # Print the numbers from 1 to 7, the list [8, [], 9], and 10
        from gplogfilter import Ungroup
        for i in Ungroup([[1, 2], [3, 4, 5], [], (6, 7), [[8, [], 9], 10]]):
            print i
    """
    for group in iterable:
        for item in group:
            yield item


def EnumerateUngroup(iterable):
    """
    Generator which takes a stream of groups and flattens by one level,
    yielding pairs (i, e) where i is a group counter (from zero) and
    e is an element of the i'th group.  Here, the term 'group' means
    a sequence, iterator, or some object which supports iteration.

    EnumerateUngroup(iterable) -> iterator
        iterable -- a sequence, iterator, or some object which supports
            iteration.  Each item returned by its next() method must be
            a sequence, iterator, or iterable object.
    """
    i = 0
    for group in iterable:
        for item in group:
            yield i, item
        i += 1

#-------------------------------------------------------------------------

def TimestampInBounds(iterable, begin, end):
    """
    Generator to extract GPDB log entries in a datetime interval:
        begin <= timestamp < end

    TimestampInBounds(iterable, begin, end) -> iterator
        iterable -- a sequence, iterator, file, or other object which
            supports iteration.  Each item returned by its next() method
            must be either a string (a line of GPDB log data) or a group
            (a sequence of strings where the timestamp, if any, is at the
            beginning of the first string).
        begin -- a date or datetime giving the lower bound of the interval;
            or None for no lower bound.
        end -- a date or datetime giving the upper bound of the interval;
            or None for no upper bound.

    Example:
        # Print log entries from stdin dated 2008-05-01.
        from logfilter import FilterByTimestamp
        from datetime import date
        from sys import stdin
        for s in TimestampInBounds(stdin, date(2008,5,1), date(2008,5,2)):
            print s

    At the beginning of a log file there may be some lines with no timestamp,
    preceding the first timestamped entry.  Any such lines are skipped.
    """
    # Prepare lower bound
    if begin is None:
        begin = '0000-00-00'
    elif hasattr(begin, 'hour'):
        begin = begin.strftime('%Y-%m-%d %H:%M:%S')   # 'YYYY-MM-DD HH:MM:SS'
    else:
        begin = begin.strftime('%Y-%m-%d')

    # Prepare upper bound
    if end is None:
        end = '9999-99-99'
    elif hasattr(end, 'hour'):
        end = end.strftime('%Y-%m-%d %H:%M:%S')
    else:
        end = end.strftime('%Y-%m-%d')

    # Fetch first item from input stream.
    source = iter(iterable)
    item = source.next()

    # If first item is a string, assume input consists of individual lines.
    # Yield lines which start with a timestamp within the given bounds, plus
    # any following lines which do not have timestamps.
    if isinstance(item, basestring):
        withinbounds = False
        while True:
            if begin <= item < end:
                withinbounds = True
                yield item
            elif timestampPattern.match(item):
                withinbounds = False
            elif withinbounds:
                yield item
            item = source.next()

    # Else assume input consists of groups (i.e. sequences) of lines.
    # Yield groups in which the first line starts with a timestamp within
    # the given bounds.  Skip groups which are empty or have no timestamp.
    while True:
        if (len(item) > 0 and
            begin <= item[0] < end):
            yield item
        item = source.next()


def MatchRegex(iterable, regex):
    """
    Generator to filter a stream, selecting items in which there is a match
    for a regular expression.

    MatchRegex(iterable, include) -> iterator
        iterable -- a sequence, iterator, file, or other object which
            supports iteration.  Each item returned by its next() method
            must be either a string or a group.  Here the term 'group'
            means a sequence of strings or an iterable yielding strings.
        regex -- a regular expression string, or a regular expression
            object returned by re.compile().  The filter excludes items
            in which there is no match for the regex.

    Example:
        # Print lines from stdin which contain the string 'ERROR:'
        from logfilter import MatchRegex
        import re, sys
        for s in MatchRegex(sys.stdin, re.compile('ERROR:')):
            print s
    """
    if isinstance(regex, basestring):
        regex = re.compile(regex)

    # Yield items in which a match is found for the 'include' pattern.
    for item in iterable:
        if isinstance(item, basestring):     # item is a string
            if regex.search(item):
                yield item
        else:                                # item is a group of strings
            for s in item:
                if regex.search(s):
                    yield item
                    break


def NoMatchRegex(iterable, regex):
    """
    Generator to filter a stream, selecting items in which there is no match
    for the regular expression.

    NoMatchRegex(iterable, regex) -> iterator
        iterable -- a sequence, iterator, file, or other object which
            supports iteration.  Each item returned by its next() method
            must be either a string or a group.  Here the term 'group'
            means a sequence of strings or an iterable yielding strings.
        regex -- a regular expression string, or a regular expression
            object returned by re.compile().  The filter excludes items
            in which there is a match for the regex.

    Example:
        # Print log entries from stdin which don't contain the string 'HINT'
        from logfilter import FilterNoMatch, GroupByTimestamp, Ungroup
        import sys
        for s in Ungroup(NoMatchRegex(TimestampInBounds(sys.stdin), 'HINT')):
            print s
    """
    if isinstance(regex, basestring):
        exclude = re.compile(regex)

    # Yield items in which no match is found for the 'exclude' pattern.
    for item in iterable:
        if isinstance(item, basestring):     # item is a string
            if not regex.search(item):
                yield item
        else:                                # item is a group of strings
            for s in item:
                if regex.search(s):
                    break
            else:
                yield item


def MatchInFirstLine(iterable, regex):
    """
    Generator to filter a stream of groups.  Yields those groups whose
    first line contains a match for the given regex.

    MatchInFirstLine(iterable, regex) -> iterator
        iterable -- a sequence, iterator, or some object which supports
            iteration.  Each item returned by its next() method must be
            a group.  Here the term 'group' means a sequence of strings.
        include -- either a string specifying a regular expression, or a
            regular expression object returned by re.compile()

    Example:
        # Print log entries whose first line contains the string 'ERROR:'
        for s in Ungroup(MatchInFirstLine(GroupByTimestamp(sys.stdin), 'ERROR:')):
            print s
    """
    if isinstance(regex, basestring):
        regex = re.compile(regex)
    for group in iterable:
        if (len(group) > 0 and
            regex.search(group[0])):
            yield group


def NoMatchInFirstLine(iterable, regex):
    """
    Generator to filter a stream of groups.  Skips those groups whose
    first line contains a match for the given regex; yields all other
    groups.

    NoMatchInFirstLine(iterable, regex) -> iterator
        iterable -- a sequence, iterator, or some object which supports
            iteration.  Each item returned by its next() method must be
            a group.  Here the term 'group' means a sequence of strings.
        regex -- either a string specifying a regular expression, or a
            regular expression object returned by re.compile()

    Example:
        # Print log entries whose first line does not contain 'DEBUGn:'
        for s in Ungroup(NoMatchInFirstLine(GroupByTimestamp(sys.stdin), r'DEBUG\d:')):
            print s
    """
    if isinstance(regex, basestring):
        regex = re.compile(regex)
    for group in iterable:
        if (len(group) == 0 or
            regex.search(group[0]) is None):
            yield group


#-------------------------- Utility Functions --------------------------

def filterize(Filter, *args, **kwargs):
    """
    Return a function of one argument (an input stream) which, when called,
    will return the result of applying the given Filter function to that
    argument together with any additional args and kwargs.

    This is useful for building a 'filters' list to be passed to FilterLogFile.

    Example:
        # Print log entries containing the string 'Detail:'
        from logfilter import FilterLogEntries, MatchRegex, filterize
        import sys
        filters = []
        filters.append(filterize(MatchRegex, 'Detail:'))
        for line in FilterLogFile(sys.stdin, filters=filters):
            print line.rstrip()
    """
    if args or kwargs:
        return lambda(stream): Filter(stream, *args, **kwargs)
    else:
        return Filter


def spiffInterval(begin=None, end=None, duration=None):
    """
    Determine a datetime interval given zero or more of the parameters
    begin, end, duration.

    The begin and end parameter values should be instances of the
    datetime.datetime or datetime.date class, or None.  Any dates
    (datetime.date) are converted to datetimes (datetime.datetime)
    with time set to 00:00:00.

    The duration parameter value should be an instance of the
    datetime.timedelta class, or None.  The duration is used to
    calculate a missing endpoint in case begin or end is None.
    The duration parameter is ignored if the caller specifies
    both begin and end.

    Returns a pair (begin, end) in which each element is either
    an instance of the datetime.datetime class or None.
    """
    if begin and not hasattr(begin, 'hour'):
        begin = datetime(begin.year, begin.month, begin.day)
    if end and not hasattr(end, 'hour'):
        end = datetime(end.year, end.month, end.day)

    if (begin is None or end is None) and duration is not None:
        if begin:
            end = begin + duration
        elif end:
            begin = end - duration
        else:
            # Neither begin nor end was given.  Let default interval
            # begin at the current date and time minus the duration.
            # Let the interval remain unbounded at the high end.
            begin = datetime.now() - duration

    return begin, end

