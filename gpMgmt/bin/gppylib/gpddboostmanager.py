#!/usr/bin/env python
#
# Copyright (c) Greenplum Inc 2012. All Rights Reserved. 
#
#

try:
    # THIS IMPORT MUST COME FIRST    
    # import mainUtils FIRST to get python version check
    from gppylib.mainUtils import *
    import sys, re
    from sets import Set
    from gppylib import gplog
    from getpass import getpass
    from optparse import OptionGroup
    from gppylib.db import dbconn
    from gppylib.gparray import GpArray
    from gppylib.gpparseopts import OptParser

except ImportError, e:
    sys.exit('Cannot import modules.  Please verify that you have sourced greenplum_path.sh.  Detail: ' + str(e))

logger = gplog.get_default_logger()

#
# This class provides all the needed functionality for the DDBoost management 
# e.g. Validate DDBoost management options,
#      configure the segment hosts with DDBoost credentials...
#
class GpDdboostManager():
    def __init__(self, options, master_port, only_ddboost_options):
        self.master_port = master_port
        self.use_ddboost = options.use_ddboost
        self.ddboost_user = options.ddboost_user
        self.ddboost_config_remove = options.ddboost_config_remove
        self.only_ddboost_options = only_ddboost_options
        self.ddboost_verify = options.ddboost_verify
        self.list_su = options.list_su
        self.create_su = options.create_su

        # This variable indicates whether we need to exit after finishing DDBoost configurations 
        # e.g. verifying the DDBoost credentials 
        self.ddboost_configure_and_exit = False

        # This is done only since 'ddboost_hosts' is expected to be a list
        if options.ddboost_hosts is None:
            self.ddboost_hosts = []
        else:
            self.ddboost_hosts = options.ddboost_hosts
        
        # If one of these flags is specified, we need to exist after the DDBoost related operations
        self.managment_operations = ((self.list_su is True) or
                                     (self.create_su is not None) or
                                     (self.ddboost_verify is True) or
                                     (self.ddboost_user is not None) or
                                     (self.ddboost_config_remove is True))

    # Return values:
    #    True  - DDBoost related operation were made (gpcrondump should exit)
    #    False - No DDBoost related operations were made (continue to other backup\restore actions).
    def execute(self):
        if bool(self.ddboost_hosts) or bool(self.ddboost_user):
            # check that both user and password are specified
            if not (bool(self.ddboost_hosts) and bool(self.ddboost_user)):
                raise ExceptionNoStackTraceNeeded('for ddboost config, both options are required --ddboost-host <ddboost_hostname> --ddboost-user <ddboost_user>.')

            # check that no other gpcrondump option was specified
            if self.only_ddboost_options and not self.ddboost_config_remove:
                self._validateUserName(userName = self.ddboost_user)
                hostset = self._getHostSet(self._getGpArray())
                p = getpass()
                self._validatePassword(password = p)
                self._sshToHosts(hostset, self.ddboost_hosts, self.ddboost_user, p)
                 # Since --ddboost-host and --ddboost-user are standalone - verify credentials and then exit
                if (self.ddboost_verify is True) or (self.create_su is not None) or (self.list_su is True):
                    self.ddboost_configure_and_exit = True
                else:
                    # If no verification is needed - just exit
                    return True
            else:
                raise ExceptionNoStackTraceNeeded('The options --ddboost-host and --ddboost-user are standalone. They are NOT used in conjunction with any other gocrondump options.')

        if (self.create_su is not None):
            if self.only_ddboost_options is True:
                self._createStorageUnit(self.create_su)
                self.ddboost_configure_and_exit = True
            else:                
                raise ExceptionNoStackTraceNeeded('The option --ddboost-create-su should be used with ddboost related options only.')

        # If --ddboost-verify or --ddboost is specified, we check the credentials that stored on the Master
        # In case of --ddboost we force the verification, which will create a storage unit if doesn't exist (MPP-17510)
        if bool(self.ddboost_verify) or bool(self.use_ddboost):
            self._ddboostVerify()
        
        # List all available storage units and then exit 
        if self.list_su is True:
            if self.only_ddboost_options is True:
                self._listStorageUnits()
                self.ddboost_configure_and_exit = True
            else:
                raise ExceptionNoStackTraceNeeded('The option --ddboost-list-su should be used with ddboost related options only.')

        # Check if we need to exit now
        if self.ddboost_configure_and_exit is True:
            return True

        # The ddboost_config_remove actually configures the ddboost with dummy values
        if self.ddboost_config_remove:
            if self.only_ddboost_options:
                hostset = self._getHostSet(self._getGpArray())
                # The username and the password must be at least 2 characters long, so we just use 2 spaces for each
                self._sshToHosts(hostset, [' '],'  ','  ')
                return True;
            else:
                raise ExceptionNoStackTraceNeeded('The option --ddboost-config-remove is standalone. It is NOT used in conjunction with any other gocrondump options.')

        # No DDBoost operations were made.
        # Continue to other backup\restore actions
        return False;


    # List all available storage units on all DD servers    
    def _listStorageUnits(self):
        cmdLine = os.environ['GPHOME'] + '/bin/gpddboost --list-su'
        logger.debug("Executing gpddboost to list available storage units: %s" % cmdLine)
        if os.system(cmdLine):
            raise ExceptionNoStackTraceNeeded('Failed to list available storage units')

    
    # Create the desired storage unit on all DD servers
    def _createStorageUnit(self, storage_unit):
        cmdLine = os.environ['GPHOME'] + '/bin/gpddboost --create-su ' + storage_unit
        logger.debug("Executing gpddboost to create storage unit '%s': %s" % (storage_unit, cmdLine))
        if not os.system(cmdLine):
            logger.info("The storage unit has been created")
        else:
            raise ExceptionNoStackTraceNeeded('Failed to create a storage unit')


    def _getGpArray(self):
        return GpArray.initFromCatalog(dbconn.DbURL(port=self.master_port), utility=True)    

    
    def _getHostSet(self, gparray):
        hostlist = gparray.get_hostlist(includeMaster=True)
        hostset = Set(hostlist)
        return hostset


    def _validateUserName(self, userName):
        if not re.search(r'^[a-zA-Z0-9-_]{1,30}$', userName):
            legal_username_str = """
            The username length must be between 1 to 30 characters.
            
            The following characters are allowed:
                1) Lowercase letters (a-z)
                2) Uppercase letters (A-Z) 
                3) Numbers (0-9)
                4) Special characters (- and _)
                
            Note: whitespace characters are not allowed.
            """
            raise ExceptionNoStackTraceNeeded(legal_username_str)


    def _sshToHosts(self, gpHostsSet, ddboostHostsSet, user, password):
        logger.debug("DDboost host(s): %s" % ddboostHostsSet)
        numberOfDdboostNics = len(ddboostHostsSet)

        # This for loops over all the segment hosts and executes appropriate 'gpddboost --setCredential' command
        for index, host in enumerate(gpHostsSet):                          
            # If more then one NIC is specified, we configure the GP hosts with these ddboost NICs in a round robin manner
            currentDdboostHost = ddboostHostsSet[index % numberOfDdboostNics]            
            ddcmd = 'source ' + os.environ['GPHOME'] + '/greenplum_path.sh; ' + os.environ['GPHOME'] + \
                    '/bin/gpddboost  --setCredential --hostname=' + currentDdboostHost + ' --user=' + re.escape(user)
            logger.debug("running gpddboost on host %s: %s --password=*" % (host, ddcmd))

            # Note: the password should not be printed to log/stdout and therefore was concatenated to ddcmd only after printing it to log
            ddcmd += " --password=" + re.escape(password)
            cmdline = 'gpssh -h %s \'%s\'' % (host, ddcmd)
            # TODO: capture the output of the 'gpddboost' (e.g. with 'subprocess.Popen()' instead of 'os.system()')
            if not os.system(cmdline):
                logger.info("config delivered successfully to %s " % host)
            else:
                logger.warn("problem delivering the config to %s " % host)

        logger.debug("done with sshToHosts. credentials were set on all servers")


    # Verify the DDBoost credentials using the credentials that stored on the Master
    # TODO: verify also all the hosts in self.ddboost_hosts (ping to the hosts, I think there is some Python function for that)
    def _ddboostVerify(self):
        verifyCmdLine = os.environ['GPHOME'] + '/bin/gpddboost --verify'
        logger.debug("Executing gpddboost to verify DDBoost credentials: %s" % verifyCmdLine)
        if not os.system(verifyCmdLine):
            logger.info("The specified DDBoost credentials are OK")
        else:
            raise ExceptionNoStackTraceNeeded('Failed to connect to DD_host with DD_user and the DD_password')

  
    def _validatePassword(self, password):
        if not re.search(r'^[a-zA-Z0-9!@#$%^&+=\*\\/\(\)-_~;:\'\"<>\{\}\[\]|\?\.\,`"]{1,40}$', password):
            legal_password_str = """
            The password length must be between 1 to 40 characters.
            
            The following characters are allowed:
                1) Lowercase letters (a-z) 
                2) Uppercase letters (A-Z) 
                3) Numbers (0-9)
                4) Special characters (! @ # $ % ^ & + = * \ / - _ ~ ; : ' " { [ ( < > ) ] } | ? . and ,). 
            """
            raise ExceptionNoStackTraceNeeded(legal_password_str)


##### GpDdboostManager class end ##### 


def add_ddboost_options_to_parser(parser):    

    # Create DDBoostConfig options
    ddConfigOpt = OptionGroup(parser, "DDBoostConfig")

    # ddboost-host may include more then one host
    ddConfigOpt.add_option('--ddboost-host', dest='ddboost_hosts', action='append', default=None,
                           help="Configuration of ddboost hostname.")
    ddConfigOpt.add_option('--ddboost-user', dest='ddboost_user', action='store', default=None, 
                           help="Configuration of ddboost user.")
    ddConfigOpt.add_option('--ddboost-config-remove', dest='ddboost_config_remove', action='store_true', default=False,
                           help="Remove ~/DDBOOST_CONFIG file.")
    ddConfigOpt.add_option('--ddboost-verify', dest='ddboost_verify', action='store_true', default=False, 
                           help="Verify DDBoost credentials on DDBoost host.")
    ddConfigOpt.add_option('--ddboost-list-su', dest='list_su', action='store_true', default=False, 
                           help="List all the storage units on all DD servers.")
    ddConfigOpt.add_option('--ddboost-create-su', dest='create_su', action='store', default=None, 
                           help="Create the given SU on on all DD servers.")

    parser.add_option_group(ddConfigOpt)

