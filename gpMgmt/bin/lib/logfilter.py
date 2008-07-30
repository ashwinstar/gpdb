#!/usr/bin/env python
#
# $Id: $

from datetime import date, datetime, time, timedelta
import re
import sys
import time

# This pattern matches the date and time stamp at the beginning of a line
# in a GPDB log file.  The timestamp format is: YYYY-MM-DD HH:MM:SS[.frac]
# A timezone specifier may follow the timestamp, but we ignore that.
timestampPattern = re.compile(r'\d\d\d\d-\d\d-\d\d \d\d:\d\d:\d\d(\.\d*)?')


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
        from gplogfilter import GroupByTimestamp
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
    source = iter(iterable)
    group = iter(source.next())
    while True:
        try:
            yield group.next()
        except StopIteration:
            group = iter(source.next())


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
    source = iter(iterable)
    group = iter(source.next())
    i = 0
    while True:
        try:
            yield i, group.next()
        except StopIteration:
            i += 1
            group = iter(source.next())


def FilterMatch(iterable, include):
    """
    Generator to filter a stream, selecting items in which an 'include'
    regex is matched.

    FilterMatch(iterable, include) -> iterator
        iterable -- a sequence, iterator, file, or other object which
            supports iteration.  Each item returned by its next() method
            must be either a string or a group.  Here the term 'group'
            means a sequence of strings or an iterable yielding strings.
        include -- a regular expression string, or a regular expression
            object returned by re.compile(), or None.  If specified, the
            filter excludes items in which there is no match for the regex.
        exclude -- a regular expression string, or a regular expression
            object returned by re.compile(), or None.  If specified, the
            filter excludes items in which there is a match for the regex.

    Example:
        # Print lines from stdin which contain the string 'ERROR:'
        from gplogfilter import FilterByPattern
        import re, sys
        for s in FilterMatch(sys.stdin, re.compile('ERROR:')):
            print s
    """
    source = iter(iterable)
    if isinstance(include, basestring):
        include = re.compile(include)
    assert include is None or hasattr(include, 'search')

    # Yield items in which a match is found for the 'include' pattern.
    while True:
        item = source.next()
        if not include:                      # no pattern, let all items go thru
            yield item
        elif isinstance(item, basestring):   # item is a string
            if include.search(item):
                yield item
        else:                                # item is a group of strings
            for s in item:
                if include.search(s):
                    yield item
                    break


def FilterNoMatch(iterable, exclude):
    """
    Generator to filter a stream, selecting items in which no 'exclude'
    regex is matched.

    FilterNoMatch(iterable, exclude) -> iterator
        iterable -- a sequence, iterator, file, or other object which
            supports iteration.  Each item returned by its next() method
            must be either a string or a group.  Here the term 'group'
            means a sequence of strings or an iterable yielding strings.
        exclude -- a regular expression string, or a regular expression
            object returned by re.compile(), or None.  If specified, the
            filter excludes items in which there is a match for the regex.

    Example:
        # Print log entries from stdin which don't contain the string 'HINT'
        from gplogfilter import FilterNoMatch, GroupByTimestamp, Ungroup
        import sys
        for s in Ungroup(FilterNoMatch(GroupByTimestamp(sys.stdin), 'HINT')):
            print s
    """
    source = iter(iterable)
    if isinstance(exclude, basestring):
        exclude = re.compile(exclude)
    assert exclude is None or hasattr(exclude, 'search')

    # Yield items in which no match is found for the 'exclude' pattern.
    while True:
        item = source.next()
        if not exclude:                      # no pattern, let all items go thru
            yield item
        elif isinstance(item, basestring):   # item is a string
            if not exclude.search(item):
                yield item
        else:                                # item is a group of strings
            for s in item:
                if exclude.search(s):
                    break
            else:
                yield item


def FilterByTimestamp(iterable, begin, end):
    """
    Generator to extract GPDB log entries in a datetime interval:
        begin <= timestamp < end

    FilterByTimestamp(iterable, begin, end) -> iterator
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
        from gplogfilter import FilterByTimestamp
        from datetime import date
        from sys import stdin
        for s in FilterByTimestamp(stdin, date(2008,5,1), date(2008,5,2)):
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
    # Yield lines which start with a timestamp within the given bounds, and
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
        self.n = 0

    def __iter__(self):
        return self

    def next(self):
        item = self.source.next()
        self.n += 1

        if isinstance(item, basestring):     # ungrouped input
            s = item                         # item is a string
        elif len(item) > 0:                  # grouped input
            s = item[0]                      # item is a sequence of strings
        else:                                # item is an empty sequence
            s = ''

        if self.minstamp > s:
            m = timestampPattern.match(s)
            if m:
                self.minstamp = m.group(0)
        if self.maxstamp < s:
            m = timestampPattern.match(s)
            if m:
                self.maxstamp = m.group(0)
        return item

    def linecount(self):
        return self.n

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


def filterOneLogFile(iterable,
                     fileOut=sys.stdout,
                     fileMsg=sys.stderr,
                     begin=None,
                     end=None,
                     include=None,
                     exclude=None,
                     verbose=False):
    """
    Read GPDB log entries from iterable (which could be a sequence or an
    already-open input file).  Write to fileOut those entries which satisfy
    the search criteria, and skip the rest.  If verbose is True, write
    status messages to fileMsg.

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
    regex which matches in no line of the entry.

    exclude is like include, except that it causes a log entry to be skipped
    if there is an exclude regex which matches in some line of the entry.

    Regular expression syntax is at http://docs.python.org/lib/re-syntax.html

    At the beginning of a log file there may be some lines with no timestamp,
    preceding the first timestamped line.  Such lines are skipped if begin or
    end is not None.
    """
    if fileMsg is None:
        verbose = false

    if verbose:
        msg = ('requested timestamp range from %s to %s'
               % (begin or 'beginning of data', end or 'end of data'))
        print >>fileMsg, msg
        iterable = spyIn = TimestampSpy(iterable)

    if begin or end:
        iterable = FilterByTimestamp(iterable, begin, end)

    if include or exclude:
        # We want patterns to be tested entry-by-entry rather than line-by-line,
        # so group together the lines of each entry.
        iterable = GroupByTimestamp(iterable)

        # Include matching log entries.
        if (isinstance(include, basestring) or
            hasattr(include, 'search')):     # one string or compiled regex
            iterable = FilterMatch(iterable, include)
        elif include:                        # a list: all must match
            for regex in include:
                iterable = FilterMatch(iterable, regex)

        # Exclude non-matching log entries.
        if (isinstance(exclude, basestring) or
            hasattr(exclude, 'search')):     # one string or compiled regex
            iterable = FilterNoMatch(iterable, exclude)
        elif exclude:                        # a list: all must fail to match
            for regex in exclude:
                iterable = FilterNoMatch(iterable, regex)

        # Break the groups back down into lines for output.
        iterable = Ungroup(iterable)

    if verbose:
        iterable = spyOut = TimestampSpy(iterable)

    for line in iterable:
        print >>fileOut, line.rstrip()

    if verbose:
        str_rangeIn = spyIn.str_range()
        str_rangeOut = spyOut.str_range()
        if str_rangeIn:
            print >>fileMsg, spyIn.linecount(), 'lines in;  timestamps from', str_rangeIn[0], 'to', str_rangeIn[1]
        else:
            print >>fileMsg, spyIn.linecount(), 'lines in;  no timestamps found'
        if str_rangeOut:
            print >>fileMsg, spyOut.linecount(), 'lines out; timestamps from', str_rangeOut[0], 'to', str_rangeOut[1]
        else:
            print >>fileMsg, spyOut.linecount(), 'lines out'


def spiffInterval(begin=None, end=None, duration=None):
    """
    Returns a tuple (begin, end) where begin and end are datetimes (instances
    of the datetime.datetime class) or None.

    If the given begin or end values are dates (datetime.date), they are
    converted to datetimes (datetime.datetime) with time set to 00:00:00.

    If duration is specified, it is used to supply a missing endpoint in
    case begin or end is None.  The duration parameter is ignored if the
    caller specifies both begin and end.
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

