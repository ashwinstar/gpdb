select count(*) from sale group by rollup((cn,vn),(pn,dt)); -- record type issue
