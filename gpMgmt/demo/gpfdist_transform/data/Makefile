all: dblp joost 

dblp: dblp.xml dblp.dtd

dblp.xml: dblp.xml.gz
	gunzip -c $< > $@

dblp.xml.gz:
	wget http://www.cs.washington.edu/research/xmldatasets/data/dblp/dblp.xml.gz

dblp.dtd:
	wget http://www.cs.washington.edu/research/xmldatasets/data/dblp/dblp.dtd

joost: joost.jar

joost.jar: joost-0.9.1-bin.zip
	unzip -p joost-0.9.1-bin.zip joost-0.9.1/joost.jar > joost.jar

joost-0.9.1-bin.zip:
	wget http://sourceforge.net/projects/joost/files/joost/0.9.1/joost-0.9.1-bin.zip/download

clean:
	-rm -f *.xml *.jar *.zip *.dtd *.gz

