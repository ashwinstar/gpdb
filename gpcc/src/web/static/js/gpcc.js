/**
 * This function gathers form element variables into an array that
 * is embedded into the current "this" variable as "this.vars". It
 * is normally used in conjunction with form() and putForm() but can
 * be used standalone as long as an image is not used for submission.
 *
 * Standalone usage examples:
 *
 * 1. Gather form vars and return array to LHS variable.
 *    var myform = $('#form-id').getForm();
 *
 * 2. Provide a serialized URL-ready string (after 1. above).
 *    var mystring = $.param(myform.vars);
 *
 * 3. Gather form vars and send to RHS plugin via "this.vars".
 *    $('#form-id').getForm().some_other_plugin();
 *
 * @return         "this" object
 * @see            form(), putForm()
 * @author         Mark Constable (markc@renta.net)
 * @author         G. vd Hoven, Mike Alsup, Sam Collett
 * @version        20060606
 */
$.fn.getForm = function() {
  var a = [];
  var ok = {INPUT:true, TEXTAREA:true, OPTION:true};
  $('*', this).each(function() {
    if (this.disabled || this.type == 'reset'
     || (this.type == 'checkbox' && !this.checked)
     || (this.type == 'radio' && !this.checked)) return;

    if (this.type == 'submit' || this.type == 'image') {
      if (this.form.clicked != this) return;
      if (this.type == 'image') {
        if (this.form.clicked_x) {
          a.push({name: this.name+'_x', value: this.form.clicked_x});
          a.push({name: this.name+'_y', value: this.form.clicked_y});
          return;
        }
      }
    }
    if (!ok[this.nodeName.toUpperCase()]) return;
    var par = this.parentNode;
    var p = par.nodeName.toUpperCase();
    if ((p == 'SELECT' || p == 'OPTGROUP') && !this.selected) return;
    var n = this.name || par.name;
    if (!n && p == 'OPTGROUP' && (par = par.parentNode))
      n = par.name;
    if (n == undefined) return;
    a.push({name: n, value: this.value});
  });
  this.vars = a;
  return this;
}

String.prototype.trim = function () {
  return this.replace(/^\s*/, "").replace(/\s*$/, "");
}

//-------------------------------------------------------------------
// isBlank(value)
//   Returns true if value only contains spaces
//-------------------------------------------------------------------
function isBlank(val){
	if(val==null){return true;}
	for(var i=0;i<val.length;i++) {
		if ((val.charAt(i)!=' ')&&(val.charAt(i)!="\t")&&(val.charAt(i)!="\n")&&(val.charAt(i)!="\r")){return false;}
		}
	return true;
}

//-------------------------------------------------------------------
// isDigit(value)
//   Returns true if value is a 1-character digit
//-------------------------------------------------------------------
function isDigit(num) {
	if (num.length>1){return false;}
	var string="1234567890";
	if (string.indexOf(num)!=-1){return true;}
	return false;
}

//-------------------------------------------------------------------
// isInteger(value)
//   Returns true if value contains all digits
//-------------------------------------------------------------------
function isInteger(val){
	if (isBlank(val)){return false;}
	for(var i=0;i<val.length;i++){
		if(!isDigit(val.charAt(i))){return false;}
		}
	return true;
}

function isPath(val) {
	 if (isBlank(val)) return false;
	 if (val.charAt(0) != '/') return false;
	 return true;
}

function isPort(val) {
	 if (! isInteger(val)) return false;
	 var intval = parseInt(val);
	 if (intval <= 0 || intval > (1 << 16)) return false;
	 return true;
}

function isValidNode(val) {
  if (isBlank(val)) return false;
  return true;
}


function isPathList(val) {
  var i;
  var paths;
  var path;
  
  if (isBlank(val)) return false;
  paths = val.split(" ");
  for (i = 0 ; i < paths.length ; i++) {
    path = paths[i].trim();
    if (path == "")
      continue;
    if (! isPath(path))
      return false;
  }
  return true;

}
