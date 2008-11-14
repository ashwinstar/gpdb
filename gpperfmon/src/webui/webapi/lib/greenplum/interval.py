import os, sys, datetime, time, math

DEBUG = False

granularity = ['1min', '5min', '1hr', '6hr', '1day', '1wk', '6mth']
seconds_per_interval_array = [60, 300, 3600, 21600, 86400, 604800, 7776000]

def check_valid(date):
    # convert time into datetime 
    (y, m, d, hr, min, sec) = time.strptime(date, '%Y-%m-%d %H:%M:%S')[:6]
    result = datetime.datetime(y, m, d, hr, min, sec)

    return result


def calc_samples(start, end, interval):
    try:
        start_date = check_valid(start)
        end_date = check_valid(end)
    except ValueError:
        return (False, 0, 0)

    # convert to seconds
    start_seconds = time.mktime(start_date.timetuple())
    end_seconds = time.mktime(end_date.timetuple())

    # Figure out how many seconds in the specified interval
    seconds_per_interval = 0;
    interval_code = 0;
    for index, item in enumerate(granularity):
        if interval==item:
            seconds_per_interval = seconds_per_interval_array[index]
            interval_code = index
    if seconds_per_interval == 0:
      return (False, 0, 0)         

    duration = end_seconds-start_seconds;
    samples = math.ceil(duration / seconds_per_interval);

    if DEBUG:
        sys.stderr.write("Interval: seconds_per_interval = %s\n" % seconds_per_interval)
        sys.stderr.write("          samples = %s\n\n" % samples)

    return (True, interval_code, samples)

if __name__ == '__main__':
    # example: checkInterval('2008-10-11 10:24:08', '2008-10-11 10:59:08', 'minute')
    pass
