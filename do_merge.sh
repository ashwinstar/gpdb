#!/bin/bash


pgfile="MERGE_PG"
pgdir="$HOME/MERGE/postgresql-8.0.3/src"
cdbdir="$HOME/MERGE/cdb2/cdb-pg/src"

ls -l $pgdir $cdbdir 
# get a clean list of the postgres files
find $pgdir | grep -v CVS | sort -u > $pgfile 

# for each of the files on the postgres side 
# find the cdb2 corollary and diff it. 
diffout="DIFF_OUT"
# nuke the old diff output file
rm -f $diffout
# make sure you handle all cases
# 	- pgfile only 
# 	- cdb2file only
#	- cousins that need merging
 
for i in `cat $pgfile` ; do 
# diff the stuff in pgdir with its cdb2 cousin
	cdbfile=$cdbdir/${i#$pgdir/}
	if [ ! -e $cdbfile ]; then 
		echo "file $cdbfile not found"
	else 
		echo "diff -buB $i $cdbfile | grep -v CVS "
		diff -buB $i $cdbdir/${i#$pgdir/} >> $diffout 2>&1
	fi
done



