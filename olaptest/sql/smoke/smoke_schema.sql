-- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
--
-- qadbschema.sql
--
--     Creates a qa schema for qa testing.  This includes the cvp cluster (Tiny)
--     as well as many other tables.
--
-- See cdbREADME.txt for more information.
--
-- $Id$
--
-- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

-- Clean up from last time.

\echo
\echo Please ignore the ** psql:cvpschema.sql:NN: ERROR:  table "<tablename>" does not exist ** errors
\echo

drop table sale cascade;
drop table product cascade;
drop table vendor cascade;
drop table customer cascade;
drop table util cascade;
drop table leagues cascade;
drop table divisions cascade;
drop table teams cascade;
drop table positions cascade;
drop table races cascade;
drop table drugs cascade;
drop table skills cascade;
drop table pregame_meals cascade;
drop table superstitions cascade;
drop table mlb_player_fact cascade;
drop table max_cols cascade;
drop table dtype_text cascade;
drop table dtype_varchar cascade;
drop table dtype_char cascade;
drop table dtype_int cascade;
drop table dtype_int2 cascade;
drop table dtype_int8 cascade;
drop table dtype_numeric cascade;
drop table dtype_float cascade;
drop table dtype_float4 cascade;
drop table dtype_date cascade;
drop table dtype_time cascade;
drop table dtype_timestamp cascade;
drop table dtype_interval cascade;
drop table dtype_boolean cascade;
drop table all_dtypes cascade;


\echo 
\echo Creating schema tables
\echo 

create table customer 
(
	cn int not null,
	cname text not null,
	
	primary key (cn)
	-- , unique (cname)
);

create table vendor 
(
	vn int not null,
	vname text not null,
	loc text,
	
	primary key (vn)
	-- , unique (vname)
);

create table product 
(
	pn int not null,
	pname text not null,
	
	primary key (pn)
	-- , unique (pname)
);

create table sale
(
	cn int not null,
	vn int not null,
	pn int not null,
	dt date not null,
	qty int not null,
	prc float not null,
	
	primary key (cn,vn,pn)
);

create table util
(
	xn int not null,
	
	primary key (xn)
);

create table leagues
(	League_ID  char(1),
	League_Name  varchar(20),
	
	primary key (League_ID)
	-- , unique (League_Name)
	);

create table divisions
(	Division_ID  varchar(10),
	Division_Name  varchar(30),
	
		
	primary key (Division_ID)
	-- , unique (Division_Name)
	);
	
create table teams
(	Team_ID  varchar(5),
	Team_Name  varchar(20),
	
	primary key (Team_ID)
	-- , unique (Team_Name)
	);
	

create table positions
(	Position_ID  numeric(1),
	Position_Name  varchar(20),
	
	primary key (Position_ID)
	-- , unique (Position_Name)	
	); 

create table races (	Race_ID  int,
	Race  varchar(20),
	
	primary key (Race_ID)
	-- , unique (Race)	
	);
	
create table drugs
(	Drug_of_Choice_ID  int2,
	Drug_Name  varchar(25),
	
	primary key (Drug_of_Choice_ID)
	-- , unique (Drug_Name)
	);
	
create table skills
(	Best_Skill_ID  int8,
	Skill  varchar(20),
	
	primary key (Best_Skill_ID)
	-- , unique (Skill)	
	);
	
create table pregame_meals
(	Pregame_Meal_ID  numeric,
	Pregame_Meal  varchar(20),
	
	primary key (Pregame_Meal_ID)
	-- , unique (Pregame_Meal)	
	);
	
create table superstitions
(	Superstition_ID  int,
	Superstition  varchar(30),
	
	primary key (Superstition_id)
	-- , unique (Superstition)	
	);
	
create table mlb_player_fact
(	Player_Name Varchar(40),
        League_ID  char(1),
	Division_ID  varchar(10),
	Team_ID  varchar(5),
	Position_ID  numeric(1),
	Race_ID  int,
	Drug_of_Choice_ID  int2,
	Best_Skill_ID  int8,
	Pregame_Meal_ID   numeric,
	Superstition_ID  int,
	Number_of_Abs  int,
	Number_of_Hits  int,
	Number_of_Hrs  int,
	Number_of_triples  int,
	Number_of_doubles  int,
	RBI int);
	
create table max_cols
(	Col_1        Numeric,
        Col_2        Numeric,
        Col_3        Numeric,
        Col_4        Numeric,
        Col_5        Numeric,
        Col_6        Numeric,
        Col_7        Numeric,
        Col_8        Numeric,
        Col_9        Numeric,
        Col_10       Numeric,                                                        
	Col_11        Numeric,
        Col_12        Numeric,
        Col_13        Numeric,
        Col_14        Numeric,
        Col_15        Numeric,
        Col_16        Numeric,
        Col_17        Numeric,
        Col_18        Numeric,
        Col_19        Numeric,
        Col_20        Numeric,        
	Col_21        Numeric,
        Col_22        Numeric,
        Col_23        Numeric,
        Col_24        Numeric,
        Col_25        Numeric,
        Col_26        Numeric,
        Col_27        Numeric,
        Col_28        Numeric,
        Col_29        Numeric,
        Col_30        Numeric,
	Col_31        Numeric,
        Col_32        Numeric,
        Col_33        Numeric,
        Col_34        Numeric,
        Col_35        Numeric,
        Col_36        Numeric,
        Col_37        Numeric,
        Col_38        Numeric,
        Col_39        Numeric,
        Col_40        Numeric,
	Col_41        Numeric,
        Col_42        Numeric,
        Col_43        Numeric,
        Col_44        Numeric,
        Col_45        Numeric,
        Col_46        Numeric,
        Col_47        Numeric,
        Col_48        Numeric,
        Col_49        Numeric,
        Col_50        Numeric,
	Col_51        Numeric,
        Col_52        Numeric,
        Col_53        Numeric,
        Col_54        Numeric,
        Col_55        Numeric,
        Col_56        Numeric,
        Col_57        Numeric,
        Col_58        Numeric,
        Col_59        Numeric,
        Col_60        Numeric,        
	Col_61        Numeric,
        Col_62        Numeric,
        Col_63        Numeric,
        Col_64        Numeric,
        Col_65        Numeric,
        Col_66        Numeric,
        Col_67        Numeric,
        Col_68        Numeric,
        Col_69        Numeric,
        Col_70        Numeric,        
	Col_71        Numeric,
        Col_72        Numeric,
        Col_73        Numeric,
        Col_74        Numeric,
        Col_75        Numeric,
        Col_76        Numeric,
        Col_77        Numeric,
        Col_78        Numeric,
        Col_79        Numeric,
        Col_80        Numeric,        	
	Col_81        Numeric,
        Col_82        Numeric,
        Col_83        Numeric,
        Col_84        Numeric,
        Col_85        Numeric,
        Col_86        Numeric,
        Col_87        Numeric,
        Col_88        Numeric,
        Col_89        Numeric,
        Col_90        Numeric,
	Col_91        Numeric,
        Col_92        Numeric,
        Col_93        Numeric,
        Col_94        Numeric,
        Col_95        Numeric,
        Col_96        Numeric,
        Col_97        Numeric,
        Col_98        Numeric,
        Col_99        Numeric,
        Col_100        Numeric,
	Col_101        Numeric,
        Col_102        Numeric,
        Col_103        Numeric,
        Col_104        Numeric,
        Col_105        Numeric,
        Col_106        Numeric,
        Col_107        Numeric,
        Col_108        Numeric,
        Col_109        Numeric,
        Col_110        Numeric,                                                        
	Col_111        Numeric,
        Col_112        Numeric,
        Col_113        Numeric,
        Col_114        Numeric,
        Col_115        Numeric,
        Col_116        Numeric,
        Col_117        Numeric,
        Col_118        Numeric,
        Col_119        Numeric,
        Col_120        Numeric,        
	Col_121        Numeric,
        Col_122        Numeric,
        Col_123        Numeric,
        Col_124        Numeric,
        Col_125        Numeric,
        Col_126        Numeric,
        Col_127        Numeric,
        Col_128        Numeric,
        Col_129        Numeric,
        Col_130        Numeric,
	Col_131        Numeric,
        Col_132        Numeric,
        Col_133        Numeric,
        Col_134        Numeric,
        Col_135        Numeric,
        Col_136        Numeric,
        Col_137        Numeric,
        Col_138        Numeric,
        Col_139        Numeric,
        Col_140        Numeric,
	Col_141        Numeric,
        Col_142        Numeric,
        Col_143        Numeric,
        Col_144        Numeric,
        Col_145        Numeric,
        Col_146        Numeric,
        Col_147        Numeric,
        Col_148        Numeric,
        Col_149        Numeric,
        Col_150        Numeric,
	Col_151        Numeric,
        Col_152        Numeric,
        Col_153        Numeric,
        Col_154        Numeric,
        Col_155        Numeric,
        Col_156        Numeric,
        Col_157        Numeric,
        Col_158        Numeric,
        Col_159        Numeric,
        Col_160        Numeric,        
	Col_161        Numeric,
        Col_162        Numeric,
        Col_163        Numeric,
        Col_164        Numeric,
        Col_165        Numeric,
        Col_166        Numeric,
        Col_167        Numeric,
        Col_168        Numeric,
        Col_169        Numeric,
        Col_170        Numeric,        
        Col_171        Numeric,
        Col_172        Numeric,
        Col_173        Numeric,
        Col_174        Numeric,
        Col_175        Numeric,
        Col_176        Numeric,
        Col_177        Numeric,
        Col_178        Numeric,
        Col_179        Numeric,
        Col_180        Numeric,        	
	Col_181        Numeric,
        Col_182        Numeric,
        Col_183        Numeric,
        Col_184        Numeric,
        Col_185        Numeric,
        Col_186        Numeric,
        Col_187        Numeric,
        Col_188        Numeric,
        Col_189        Numeric,
        Col_190        Numeric,
	Col_191        Numeric,
        Col_192        Numeric,
        Col_193        Numeric,
        Col_194        Numeric,
        Col_195        Numeric,
        Col_196        Numeric,
        Col_197        Numeric,
        Col_198        Numeric,
        Col_199        Numeric,
        Col_200        Numeric,                
	Col_201        Numeric,
        Col_202        Numeric,
        Col_203        Numeric,
        Col_204        Numeric,
        Col_205        Numeric,
        Col_206        Numeric,
        Col_207        Numeric,
        Col_208        Numeric,
        Col_209        Numeric,
        Col_210        Numeric,                                                        
	Col_211        Numeric,
        Col_212        Numeric,
        Col_213        Numeric,
        Col_214        Numeric,
        Col_215        Numeric,
        Col_216        Numeric,
        Col_217        Numeric,
        Col_218        Numeric,
        Col_219        Numeric,
        Col_220        Numeric,        
	Col_221        Numeric,
        Col_222        Numeric,
        Col_223        Numeric,
        Col_224        Numeric,
        Col_225        Numeric,
        Col_226        Numeric,
        Col_227        Numeric,
        Col_228        Numeric,
        Col_229        Numeric,
        Col_230        Numeric,
	Col_231        Numeric,
        Col_232        Numeric,
        Col_233        Numeric,
        Col_234        Numeric,
        Col_235        Numeric,
        Col_236        Numeric,
        Col_237        Numeric,
        Col_238        Numeric,
        Col_239        Numeric,
        Col_240        Numeric,
	Col_241        Numeric,
        Col_242        Numeric,
        Col_243        Numeric,
        Col_244        Numeric,
        Col_245        Numeric,
        Col_246        Numeric,
        Col_247        Numeric,
        Col_248        Numeric,
        Col_249        Numeric,
        Col_250        Numeric,
        Col_251        Numeric,
        Col_252        Numeric,
        Col_253        Numeric,
        Col_254        Numeric,
        Col_255        Numeric,
        Col_256        Numeric);

create table dtype_text (text_col  text);

create table dtype_varchar (varchar_col  varchar(10));

create table dtype_char (char_col  char(10));

create table dtype_int (int_col  int);

create table dtype_int2 (int2_col  int2);

create table dtype_int8 (int8_col  int8);

create table dtype_numeric (numeric_col  numeric);

create table dtype_float (float_col  float);

create table dtype_float4 (float4_col  float4);

create table dtype_date (date_col  date);

create table dtype_time (time_col  time);

create table dtype_timestamp (timestamp_col  timestamp);

create table dtype_interval (interval_col  interval);

create table dtype_boolean (boolean_col  boolean);

create table all_dtypes
(text_col  text,
varchar_col  varchar(10),
char_col  char(10),
int_col  int,
int2_col  int2,
int8_col  int8,
numeric_col  numeric,
float_col  float,
float4_col  float4,
date_col  date,
time_col  time,
timestamp_col  timestamp,
interval_col  interval,
boolean_col  Boolean);

												

\echo 
\echo Granting access to PUBLIC
\echo

grant select on sale to public;
grant select on product to public;
grant select on vendor to public;
grant select on customer to public;
grant select on util to public;
grant select on leagues to public;
grant select on divisions to public;
grant select on teams to public;
grant select on positions to public;
grant select on races to public;
grant select on drugs to public;
grant select on skills to public;
grant select on pregame_meals to public;
grant select on superstitions to public;
grant select on mlb_player_fact to public;
grant select on max_cols to public;
grant select on dtype_text to public;
grant select on dtype_varchar to public;
grant select on dtype_char to public;
grant select on dtype_int to public;
grant select on dtype_int2 to public;
grant select on dtype_int8 to public;
grant select on dtype_numeric to public;
grant select on dtype_float to public;
grant select on dtype_float4 to public;
grant select on dtype_date to public;
grant select on dtype_time to public;
grant select on dtype_timestamp to public;
grant select on dtype_interval to public;
grant select on dtype_boolean to public;
grant select on all_dtypes to public;

create view vsale as select * from sale ;
create view vproduct as select * from product ;
create view vvendor as select * from vendor ;
create view vcustomer as select * from customer ;
create view vutil as select * from util ;
create view vleagues as select * from leagues ;
create view vdivisions as select * from divisions ;
create view vteams as select * from teams ;
create view vpositions as select * from positions ;
create view vraces as select * from races ;
create view vdrugs as select * from drugs ;
create view vskills as select * from skills ;
create view vpregame_meals as select * from pregame_meals ;
create view vsuperstitions as select * from superstitions ;
create view vmlb_player_fact as select * from mlb_player_fact ;
create view vmax_cols as select * from max_cols ;
create view vdtype_text as select * from dtype_text ;
create view vdtype_varchar as select * from dtype_varchar ;
create view vdtype_char as select * from dtype_char ;
create view vdtype_int as select * from dtype_int ;
create view vdtype_int2 as select * from dtype_int2 ;
create view vdtype_int8 as select * from dtype_int8 ;
create view vdtype_numeric as select * from dtype_numeric ;
create view vdtype_float as select * from dtype_float ;
create view vdtype_float4 as select * from dtype_float4 ;
create view vdtype_date as select * from dtype_date ;
create view vdtype_time as select * from dtype_time ;
create view vdtype_timestamp as select * from dtype_timestamp ;
create view vdtype_interval as select * from dtype_interval ;
create view vdtype_boolean as select * from dtype_boolean ;
create view vall_dtypes as select * from all_dtypes ;
