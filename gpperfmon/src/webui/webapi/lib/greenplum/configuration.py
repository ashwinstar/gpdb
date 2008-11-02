import os, sys, ConfigParser

end_path = '/gpperfmon/conf/gpperfmon.conf'

# Holds the complete configuration
gpperfmon_config = {}

# Holds the values of specific configuration settings we care about
requires_pw = ''

def loadconfig(datadir):
    filename = datadir + end_path
    try:
        # parse configuration file
        cfg = ConfigParser.ConfigParser()
        cfg.readfp(open(filename))
    except:
        sys.stderr.write("Unable to read configuration file: %s\n" % filename)
        return {}

    for section in cfg.sections():
        section_all = {}
        for item in cfg.items(section):
            section_all[item[0].replace(" ","_")] = item[1]
        gpperfmon_config[section.replace(" ","_")] = section_all

    sys.stderr.write("Configuration file: %s\n\n" % gpperfmon_config)


    # Now look for specific values

    try:
        requires_pw = cfg.getboolean("WEB_SERVICE","requires_pw")
    except:
        requires_pw = False
    sys.stderr.write("Conf: requires_pw = %s\n\n" % requires_pw)


    return gpperfmon_config

