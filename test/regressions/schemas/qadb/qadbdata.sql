-- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
--
-- qadbdata.sql
--
--      Sample data for the schema in qadbpschema.sql
--
-- See cdbREADME.txt for more information.
--
-- $Id$
--
-- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

-- Clean up from last time
delete from sale;
delete from customer;
delete from vendor;
delete from product;
delete from util;
delete from leagues;
delete from divisions;
delete from teams;
delete from positions;
delete from races;
delete from drugs;
delete from skills;
delete from pregame_meals;
delete from superstitions;
delete from mlb_player_fact;
delete from max_cols;
delete from dtype_text;
delete from dtype_varchar;
delete from dtype_char;
delete from dtype_int;
delete from dtype_int2;
delete from dtype_int8;
delete from dtype_numeric;
delete from dtype_float;
delete from dtype_float4;
delete from dtype_date;
delete from dtype_time;
delete from dtype_timestamp;
delete from dtype_interval;
delete from dtype_boolean;
delete from all_dtypes;



-- Customers
insert into customer values ( 1, 'Macbeth');
insert into customer values ( 2, 'Duncan');
insert into customer values ( 3, 'Lady Macbeth');
insert into customer values ( 4, 'Rozencrantz');
insert into customer values ( 5, 'Guildenstern');
insert into customer values ( 6, 'Pierce');
insert into customer values ( 7, 'Big Al');
insert into customer values ( 8, 'Antoine Walker');
insert into customer values ( 9, 'Fennis Dembo');
insert into customer values ( 10, 'Michael Jackson');


-- Vendors
insert into vendor values ( 10, 'Witches, Inc', 'Lonely Heath');
insert into vendor values ( 20, 'Lady Macbeth', 'Inverness');
insert into vendor values ( 30, 'Duncan', 'Forres');
insert into vendor values ( 40, 'Macbeth', 'Inverness');
insert into vendor values ( 50, 'Macduff', 'Fife');
insert into vendor values ( 63, 'Monsters, Inc', 'Monstropolis');
insert into vendor values ( 72, 'Greenplum', 'San Mateo');
insert into vendor values ( 81, 'Sour Apple', 'Sherman Oaks');
insert into vendor values ( 94, 'Rotten Pear', 'Bakersfield');
insert into vendor values ( 105, 'Holy Rollers', 'City of Industry');

-- Products
insert into product values ( 100, 'Sword');
insert into product values ( 200, 'Dream');
insert into product values ( 300, 'Castle');
insert into product values ( 400, 'Justice');
insert into product values ( 401, 'Sins');
insert into product values ( 402, 'Misfortune');
insert into product values ( 403, 'Classified Material');
insert into product values ( 404, 'Textbooks');
insert into product values ( 405, 'Nuclear Weapons');
insert into product values ( 406, 'Acid Rain');

-- Sales (transactions)
insert into sale values ( 2, 40, 100, '1401-1-1', 1100, 2400);
insert into sale values ( 1, 10, 200, '1401-3-1', 1, 0);
insert into sale values ( 3, 40, 200, '1401-4-1', 1, 0);
insert into sale values ( 1, 20, 100, '1401-5-1', 1, 0);
insert into sale values ( 1, 30, 300, '1401-5-2', 1, 0);
insert into sale values ( 1, 50, 400, '1401-6-1', 1, 0);
insert into sale values ( 2, 50, 400, '1401-6-1', 1, 0);
insert into sale values ( 4, 63, 401, '1401-6-3', 2, 100);
insert into sale values ( 4, 72, 403, '2005-3-3', 100, 6);
insert into sale values ( 5, 81, 404, '2005-4-3', 11, 7);
insert into sale values ( 7, 81, 404, '2005-4-3', 11, 7);
insert into sale values ( 7, 105, 405, '2005-4-1', 1, 1000);
insert into sale values ( 7, 94, 402, '2005-4-2', 1, 99);
insert into sale values ( 8, 94, 402, '2005-4-2', 1, 99);
insert into sale values ( 8, 40, 100, '1401-1-1', 1100, 2400);
insert into sale values ( 9, 40, 200, '1401-4-1', 1, 0);
insert into sale values ( 10, 105, 406, '2005-4-1', 1, 999);


-- Util
insert into util values(1);
insert into util values(20);
insert into util values(300);
insert into util values(301);
insert into util values(302);
insert into util values(303);
insert into util values(304);
insert into util values(305);
insert into util values(306);
insert into util values(307);
insert into util values(308);
insert into util values(309);
insert into util values(310);


---- Leagues ------------------
Insert into Leagues (League_ID, League_Name)
values('A', 'American League');	

Insert into Leagues (League_ID, League_Name)
values('N', 'National League');

---- Divisions ------------------
Insert into Divisions(Division_ID, Division_Name)
values ('ALWEST','American League West'); 

Insert into Divisions(Division_ID, Division_Name)
values ('ALCENTRAL','American League Central');	

Insert into Divisions(Division_ID, Division_Name)
values ('ALEAST','American League East');

Insert into Divisions(Division_ID, Division_Name)
values ('NLWEST','National League West'); 

Insert into Divisions(Division_ID, Division_Name)
values ('NLCENTRAL','National League Central');	

Insert into Divisions(Division_ID, Division_Name)
values ('NLEAST','National League East');

---- Teams ------------------
Insert into Teams(Team_ID, Team_Name)
values ('BOS','Boston Red Sox');	

Insert into Teams(Team_ID, Team_Name)
values ('NYY','New York Yankees');

Insert into Teams(Team_ID, Team_Name)
values ('CWS','Chicago White Sox');

Insert into Teams(Team_ID, Team_Name)
values ('MIN','Minnesota Twins');

Insert into Teams(Team_ID, Team_Name)
values ('ANA','Anaheim Angels');

Insert into Teams(Team_ID, Team_Name)
values ('OAK','Oakland Athletics');

Insert into Teams(Team_ID, Team_Name)
values ('SEA','Seattle Mariners');

Insert into Teams(Team_ID, Team_Name)
values ('NYM','New York Mets');

Insert into Teams(Team_ID, Team_Name)
values ('ATL','Atlanta Braves');

Insert into Teams(Team_ID, Team_Name)
values ('LA','Los Angeles Dodgers');

Insert into Teams(Team_ID, Team_Name)
values ('SF','San Fransisco');

Insert into Teams(Team_ID, Team_Name)
values ('CHI','Chicago Cubs');

Insert into Teams(Team_ID, Team_Name)
values ('STL','St. Louis Cardinals');


---- Position ------------------
Insert into Positions (Position_ID, Position_Name)
values (1,'Pitcher');	

Insert into Positions (Position_ID, Position_Name)
values (2,'Catcher');

Insert into Positions (Position_ID, Position_Name)
values (3,'First Base');

Insert into Positions (Position_ID, Position_Name)
values (4,'Second Base');

Insert into Positions (Position_ID, Position_Name)
values (5,'Third Base');

Insert into Positions (Position_ID, Position_Name)
values (6,'Short Stop');

Insert into Positions (Position_ID, Position_Name)
values (7,'Left Field');

Insert into Positions (Position_ID, Position_Name)
values (8,'Center Field');

Insert into Positions (Position_ID, Position_Name)
values (9,'Right Field');

---- Races ------------------
Insert into Races (Race_ID, Race)
values(1,'Caucasian');

Insert into Races (Race_ID, Race)
values(2,'African American');

Insert into Races (Race_ID, Race)
values(3,'Latino');

Insert into Races (Race_ID, Race)
values(4,'Asian');

Insert into Races (Race_ID, Race)
values(5,'Native American');



---- Drugs ------------------
Insert into Drugs(Drug_of_Choice_ID, Drug_Name)
values(1,'Steroids');

Insert into Drugs(Drug_of_Choice_ID, Drug_Name)
values(2,'Andro');

Insert into Drugs(Drug_of_Choice_ID, Drug_Name)
values(3,'Cocaine');

Insert into Drugs(Drug_of_Choice_ID, Drug_Name)
values(4,'Alcohol');

Insert into Drugs(Drug_of_Choice_ID, Drug_Name)
values(5,'Marijuana');

Insert into Drugs(Drug_of_Choice_ID, Drug_Name)
values(6,'Never touch the stuff');

---- Skills ------------------
Insert into Skills(Best_Skill_ID,Skill)
values(1,'Run');	

Insert into Skills(Best_Skill_ID,Skill)
values(2,'Field');

Insert into Skills(Best_Skill_ID,Skill)
values(3,'Throw');

Insert into Skills(Best_Skill_ID,Skill)
values(4,'Hitting for Avg');

Insert into Skills(Best_Skill_ID,Skill)
values(5,'Hitting for Power');

---- Pregame_Meals ------------------
Insert into Pregame_Meals(Pregame_Meal_ID, Pregame_Meal)
values(1,'Chicken and Waffles');

Insert into Pregame_Meals(Pregame_Meal_ID, Pregame_Meal)
values(2,'Lobster');

Insert into Pregame_Meals(Pregame_Meal_ID, Pregame_Meal)
values(3,'Roast Beef');

Insert into Pregame_Meals(Pregame_Meal_ID, Pregame_Meal)
values(4,'Chile Colorado');


---- Superstitions ------------------
Insert into Superstitions(Superstition_id,Superstition)
values(1,'None');	

Insert into Superstitions(Superstition_id,Superstition)
values(2,'Same meal before each game');

Insert into Superstitions(Superstition_id,Superstition)
values(3,'Do not step on white lines');

Insert into Superstitions(Superstition_id,Superstition)
values(4,'Lucky helmet');	
	


---- MLB_Player_Fact ------------------
Insert into MLB_Player_Fact
(	Player_Name,
        League_ID,
	Division_ID,
	Team_ID,
	Position_ID,
	Race_ID,
	Drug_of_Choice_ID,
	Best_Skill_ID,
	Pregame_Meal_ID,
	Superstition_ID,
	Number_of_Abs,
	Number_of_Hits,
	Number_of_Hrs,
	Number_of_triples,
	Number_of_doubles,
	RBI)
values
(	'Manny Ramirez',
        'A',
	'ALEAST',
	'BOS',
	7,
	3,
	1,
	5,
	4,
	3,
	633,
	211,
	45,
	4,
	43,
	119);
	

Insert into MLB_Player_Fact
(	Player_Name,
        League_ID,
	Division_ID,
	Team_ID,
	Position_ID,
	Race_ID,
	Drug_of_Choice_ID,
	Best_Skill_ID,
	Pregame_Meal_ID,
	Superstition_ID,
	Number_of_Abs,
	Number_of_Hits,
	Number_of_Hrs,
	Number_of_triples,
	Number_of_doubles,
	RBI)
values
(	'Derek Jeter',
        'A',
	'ALEAST',
	'NYY',
	6,
	2,
	3,
	2,
	1,
	2,
	600,
	156,
	23,
	4,
	28,
	82);


Insert into MLB_Player_Fact
(	Player_Name,
        League_ID,
	Division_ID,
	Team_ID,
	Position_ID,
	Race_ID,
	Drug_of_Choice_ID,
	Best_Skill_ID,
	Pregame_Meal_ID,
	Superstition_ID,
	Number_of_Abs,
	Number_of_Hits,
	Number_of_Hrs,
	Number_of_triples,
	Number_of_doubles,
	RBI)
values
(	'Big Hurt',
        'A',
	'ALCENTRAL',
	'CWS',
	3,
	2,
	2,
	5,
	2,
	4,
	478,
	177,
	32,
	0,
	22,
	89);
	
Insert into MLB_Player_Fact
(	Player_Name,
        League_ID,
	Division_ID,
	Team_ID,
	Position_ID,
	Race_ID,
	Drug_of_Choice_ID,
	Best_Skill_ID,
	Pregame_Meal_ID,
	Superstition_ID,
	Number_of_Abs,
	Number_of_Hits,
	Number_of_Hrs,
	Number_of_triples,
	Number_of_doubles,
	RBI)
values
(	'Rod Carew',
        'A',
	'ALCENTRAL',
	'MIN',
	3,
	2,
	5,
	4,
	4,
	1,
	611,
	256,
	4,
	11,
	44,
	77);
	
	
Insert into MLB_Player_Fact
(	Player_Name,
        League_ID,
	Division_ID,
	Team_ID,
	Position_ID,
	Race_ID,
	Drug_of_Choice_ID,
	Best_Skill_ID,
	Pregame_Meal_ID,
	Superstition_ID,
	Number_of_Abs,
	Number_of_Hits,
	Number_of_Hrs,
	Number_of_triples,
	Number_of_doubles,
	RBI)
values
(	'Vlad Guerrero',
        'A',
	'ALWEST',
	'ANA',
	9,
	2,
	4,
	3,
	3,
	5,
	680,
	222,
	52,
	11,
	43,
	133);	

	
	
Insert into MLB_Player_Fact
(	Player_Name,
        League_ID,
	Division_ID,
	Team_ID,
	Position_ID,
	Race_ID,
	Drug_of_Choice_ID,
	Best_Skill_ID,
	Pregame_Meal_ID,
	Superstition_ID,
	Number_of_Abs,
	Number_of_Hits,
	Number_of_Hrs,
	Number_of_triples,
	Number_of_doubles,
	RBI)
values
(	'Eric Chavez',
        'A',
	'ALWEST',
	'OAK',
	6,
	1,
	5,
	2,
	3,
	1,
	621,
	198,
	44,
	3,
	33,
	101);
	
Insert into MLB_Player_Fact
(	Player_Name,
        League_ID,
	Division_ID,
	Team_ID,
	Position_ID,
	Race_ID,
	Drug_of_Choice_ID,
	Best_Skill_ID,
	Pregame_Meal_ID,
	Superstition_ID,
	Number_of_Abs,
	Number_of_Hits,
	Number_of_Hrs,
	Number_of_triples,
	Number_of_doubles,
	RBI)
values
(	'Carlos Beltran',
        'N',
	'NLEAST',
	'NYM',
	8,
	3,
	4,
	1,
	2,
	3,
	577,
	176,
	32,
	5,
	44,
	95);
	
Insert into MLB_Player_Fact
(	Player_Name,
        League_ID,
	Division_ID,
	Team_ID,
	Position_ID,
	Race_ID,
	Drug_of_Choice_ID,
	Best_Skill_ID,
	Pregame_Meal_ID,
	Superstition_ID,
	Number_of_Abs,
	Number_of_Hits,
	Number_of_Hrs,
	Number_of_triples,
	Number_of_doubles,
	RBI)
values
(	'Chipper Jones',
        'N',
	'NLEAST',
	'ATL',
	6,
	1,
	2,
	3,
	1,
	1,
	398,
	126,
	31,
	1,
	22,
	67);
	
Insert into MLB_Player_Fact
(	Player_Name,
        League_ID,
	Division_ID,
	Team_ID,
	Position_ID,
	Race_ID,
	Drug_of_Choice_ID,
	Best_Skill_ID,
	Pregame_Meal_ID,
	Superstition_ID,
	Number_of_Abs,
	Number_of_Hits,
	Number_of_Hrs,
	Number_of_triples,
	Number_of_doubles,
	RBI)
values
(	'Milton Bradley',
        'N',
	'NLWEST',
	'LA',
	9,
	1,
	1,
	4,
	2,
	4,
	578,
	198,
	25,
	3,
	29,
	92);
	
	
Insert into MLB_Player_Fact
(	Player_Name,
        League_ID,
	Division_ID,
	Team_ID,
	Position_ID,
	Race_ID,
	Drug_of_Choice_ID,
	Best_Skill_ID,
	Pregame_Meal_ID,
	Superstition_ID,
	Number_of_Abs,
	Number_of_Hits,
	Number_of_Hrs,
	Number_of_triples,
	Number_of_doubles,
	RBI)
values
(	'Barry Bonds',
        'N',
	'NLWEST',
	'SF',
	7,
	1,
	1,
	5,
	1,
	2,
	555,
	222,
	77,
	1,
	22,
	199);
	
	
Insert into MLB_Player_Fact
(	Player_Name,
        League_ID,
	Division_ID,
	Team_ID,
	Position_ID,
	Race_ID,
	Drug_of_Choice_ID,
	Best_Skill_ID,
	Pregame_Meal_ID,
	Superstition_ID,
	Number_of_Abs,
	Number_of_Hits,
	Number_of_Hrs,
	Number_of_triples,
	Number_of_doubles,
	RBI)
values
(	'Nomahhhh Garciaparra',
        'N',
	'NLCENTRAL',
	'CHI',
	6,
	3,
	5,
	4,
	4,
	4,
	524,
	188,
	25,
	5,
	35,
	95);
	
Insert into MLB_Player_Fact
(	Player_Name,
        League_ID,
	Division_ID,
	Team_ID,
	Position_ID,
	Race_ID,
	Drug_of_Choice_ID,
	Best_Skill_ID,
	Pregame_Meal_ID,
	Superstition_ID,
	Number_of_Abs,
	Number_of_Hits,
	Number_of_Hrs,
	Number_of_triples,
	Number_of_doubles,
	RBI)
values
(	'David Eckstein',
        'N',
	'NLCENTRAL',
	'STL',
	6,
	1,
	5,
	2,
	2,
	3,
	578,
	175,
	7,
	8,
	29,
	47);
	
	
Insert into MLB_Player_Fact
(	Player_Name,
        League_ID,
	Division_ID,
	Team_ID,
	Position_ID,
	Race_ID,
	Drug_of_Choice_ID,
	Best_Skill_ID,
	Pregame_Meal_ID,
	Superstition_ID,
	Number_of_Abs,
	Number_of_Hits,
	Number_of_Hrs,
	Number_of_triples,
	Number_of_doubles,
	RBI)
values
(	'Ichiro Suzuki',
        'A',
	'ALWEST',
	'SEA',
	8,
	4,
	3,
	4,
	3,
	4,
	678,
	257,
	2,
	12,
	52,
	65);

--- max_cols ---
	
Insert into max_cols
values(0,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,
51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,
101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,
126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,
151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,
201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,
226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256);	

Insert into max_cols
values(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,
51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,
101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,
126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,
151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,
201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,
226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256);	

Insert into max_cols
values(2,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,
51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,
101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,
126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,
151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,
201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,
226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256);	

Insert into max_cols
values(3,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,
51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,
101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,
126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,
151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,
201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,
226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256);	

Insert into max_cols
values(4,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,
51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,
101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,
126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,
151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,
201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,
226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256);	

Insert into max_cols
values(5,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,
51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,
101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,
126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,
151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,
201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,
226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256);	

Insert into max_cols
values(6,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,
51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,
101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,
126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,
151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,
201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,
226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256);	

Insert into max_cols
values(7,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,
51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,
101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,
126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,
151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,
201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,
226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256);	

Insert into max_cols
values(8,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,
51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,
101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,
126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,
151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,
201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,
226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256);	

Insert into max_cols
values(9,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,
51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,
101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,
126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,
151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,
201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,
226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256);	

Insert into max_cols
values(10,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,
51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,
101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,
126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,
151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,
201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,
226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256);	

Insert into max_cols
values(11,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,
51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,97,98,99,100,
101,102,103,104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,
126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,144,145,146,147,148,149,150,
151,152,153,154,155,156,157,158,159,160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,198,199,200,
201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,224,225,
226,227,228,229,230,231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,256);	

--- Data Type Tables
insert into dtype_text values('text');

insert into dtype_varchar values('varchar');

insert into dtype_char values('char');

insert into dtype_int values(1);

insert into dtype_int2 values(2);

insert into dtype_int8 values(8);

insert into dtype_numeric values(100.1);

insert into dtype_float values(200.22);

insert into dtype_float4 values(300.333);

insert into dtype_date values('01/01/2005');

insert into dtype_time values('01/01/2005 23:59');

insert into dtype_timestamp values('01/02/2005 23:59');

insert into dtype_interval values('23:59:00');

insert into all_dtypes
(text_col,
varchar_col,
char_col,
int_col,
int2_col,
int8_col,
numeric_col,
float_col,
float4_col,
date_col,
time_col,
timestamp_col,
interval_col,
boolean_col)
values
('text',
'varchar',
'char',
1,
2,
8,
100.1,
200.22,
300.333,
'01/01/2005',
'01/01/2005 23:59',
'01/02/2005 23:59',
'23:59:00',
'T');

insert into dtype_boolean values('T');