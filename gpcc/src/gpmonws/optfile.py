class OptionsFile:
    options = {}

    def __init__(self, filename=''):
        if filename:
            self.load_file(filename)

    def load_file(self, filename):
        f = open(filename, 'r')
        l = ''
        while True:
            l = f.readline()
            if not l: break
            opt = l.split('=')
            if len(opt) == 2:
                self.options[opt[0].strip()] = opt[1].strip()
