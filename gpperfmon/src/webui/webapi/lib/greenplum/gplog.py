import logging
import logging.handlers

class GpWebLogger:
    def __init__(self, filename):
        self.filename = filename
        self.logger = logging.getLogger('gpmonws')
        self.logger.setLevel(logging.DEBUG)
        self.handler = logging.handlers.RotatingFileHandler(self.filename, maxBytes=1000000, backupCount=5)
        self.formatter = logging.Formatter("%(asctime)s - %(message)s")
        self.handler.setFormatter(self.formatter)
        self.logger.addHandler(self.handler)
        self.verbose = False

    def setVerbose(self):
        self.verbose = True

    def debug(self, text):
        if (self.verbose):
            self.logger.debug(text)

    def msg(self, text):
        self.logger.debug(text)

if __name__ == '__main__':
    log = GpWebLogger('out.txt')
    log.msg("Hello World 1")
