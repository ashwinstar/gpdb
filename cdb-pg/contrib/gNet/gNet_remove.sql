drop aggregate sum(int2[]) cascade;
drop aggregate sum(int4[]) cascade;
drop aggregate sum(int8[]) cascade;
drop aggregate sum(float8[]) cascade;

drop aggregate pivot_sum(text[], text, int4) cascade;
drop aggregate pivot_sum(text[], text, int8) cascade;
drop aggregate pivot_sum(text[], text, float8) cascade;

drop aggregate array_accum(anyelement) cascade;

drop function matrix_transpose(anyarray) cascade;

drop function matrix_multiply(int2[], int2[]) cascade;
drop function matrix_multiply(int4[], int4[]) cascade;
drop function matrix_multiply(int8[], int8[]) cascade;
drop function matrix_multiply(float8[], float8[]) cascade;

drop function matrix_multiply(int8[], int8) cascade;
drop function matrix_multiply(float8[], float8) cascade;

drop function matrix_add(int2[],int2[]) cascade;
drop function matrix_add(int4[],int4[]) cascade;
drop function matrix_add(int8[],int2[]) cascade;
drop function matrix_add(int8[],int4[]) cascade;
drop function matrix_add(int8[],int8[]) cascade;
drop function matrix_add(float8[],float8[]) cascade;

drop function unpivot(float8[], text[]) cascade;
drop function unpivot(int4[], text[]) cascade;

drop function pivot_accum(int4[], text[], text, int4) cascade;
drop function pivot_accum(int8[], text[], text, int8) cascade;
drop function pivot_accum(float8[], text[], text, float8) cascade;

drop aggregate mregr_coef(float8, float8[]) cascade;
drop aggregate mregr_R2(float8, float8[]) cascade;
drop function regr_R(float8[], float8[]) cascade;
drop function regr_R2(float8[], float8[]) cascade;

drop type unpivot_record_float8 cascade;
drop type unpivot_record_int4 cascade;
