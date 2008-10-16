import os, sys, datetime, time
import gpmonws

SECONDS_15=15
SECONDS_PER_MINUTE=60
SECONDS_PER_HOUR=3600
SECONDS_PER_DAY=86400
SECONDS_PER_MONTH=2592000

# default maximum data points
max_data_points = 300


def check(start, end, interval):
    try:
        # convert time into datetime 
        (y, m, d, hr, min, sec) = time.strptime(start, '%Y-%m-%d %H:%M:%S')[:6]
        start_date = datetime.datetime(y, m, d, hr, min, sec)
        (y, m, d, hr, min, sec) = time.strptime(end, '%Y-%m-%d %H:%M:%S')[:6]
        end_date = datetime.datetime(y, m, d, hr, min, sec)
    except ValueError:
        return None

    # convert to seconds
    start_seconds = time.mktime(start_date.timetuple())
    end_seconds = time.mktime(end_date.timetuple())

    data_points = max_data_points + 1

    while data_points > max_data_points:
        if interval == 'month':
            data_points = int(((end_seconds - start_seconds) / SECONDS_PER_MONTH)+1)
        elif interval == 'day':
            data_points = int(((end_seconds - start_seconds) / SECONDS_PER_DAY)+1)
        elif interval == 'hour':
            data_points = int(((end_seconds - start_seconds) / SECONDS_PER_HOUR)+1)
        elif interval == 'minute':
            data_points = int(((end_seconds - start_seconds) / SECONDS_PER_MINUTE)+1)
        elif interval == 'second':
            data_points = int(((end_seconds - start_seconds) / SECONDS_15)+1)
        else:
            # unknown interval: default to smallest interval
            sys.stderr.write('\nUnknown interval\n')
            interval = 'minute'

        if data_points > max_data_points:
            if interval == 'month':
                sys.stderr.write('\nInterval too large\n')
            if interval == 'day':
                interval = 'month'
            if interval == 'hour':
                interval = 'day'
            if interval == 'minute':
                interval = 'hour'
            if interval == 'second':
                interval = 'minute'

    return interval

if __name__ == '__main__':
    # example: checkInterval('2008-10-11 10:24:08', '2008-10-11 10:59:08', 'minute')
    pass
