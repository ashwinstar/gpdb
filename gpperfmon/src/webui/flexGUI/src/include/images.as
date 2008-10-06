// ActionScript file

/*
[Embed(source="/images/icon_accept.png")]
[Bindable]
public var icoAccept:Class;

[Embed(source="/images/icon_server.png")]
[Bindable]
public var icoServer:Class;

[Embed(source="/images/icon_magglass.png")]
[Bindable]
public var icoMagGlass:Class;

[Embed(source="/images/icon_questionmark.png")]
[Bindable]
public var icoQuestionMark:Class;

[Embed(source="/images/icon_plus.png")]
[Bindable]
public var icoPlus:Class;

[Embed(source="/images/icon_minus.png")]
[Bindable]
public var icoMinus:Class;

[Embed(source="/images/icon_folder_project.png")]
[Bindable]
public var icoProjects:Class;

[Embed(source="/images/icon_folder_cube.png")]
[Bindable]
public var icoCubes:Class;

[Embed(source="/images/icon_folder_log.png")]
[Bindable]
public var icoLogs:Class;

[Embed(source="/images/icon_check.png")]
[Bindable]
public var icoCheck:Class;

[Embed(source="/images/icon_log.png")]
[Bindable]
public var icoLog:Class;

[Embed(source="/images/icon_cube.png")]
[Bindable]
public var icoCube:Class;

[Embed(source="/images/icon_dimensions.png")]
[Bindable]
public var icoDimensions:Class;

[Embed(source="/images/icon_gears.png")]
[Bindable]
public var icoGear:Class;

[Embed(source="/images/icon_refresh.png")]
[Bindable]
public var icoRefresh:Class;

[Embed(source="/images/icon_save.png")]
[Bindable]
public var icoSave:Class;

[Embed(source="/images/icon_cancel.png")]
[Bindable]
public var icoCancel:Class;

[Embed(source="/images/icon_clear.png")]
[Bindable]
public var icoClear:Class;

[Embed(source="/images/icon_exclamation.png")]
[Bindable]
public var icoExclamation:Class;

[Embed(source="/images/icon_atomic.png")]
[Bindable]
public var icoAtomic:Class;

public function setIcons(item:Object):Class
{
	var iconClass:Class;
	var classType:String = XML(item).attribute("classType");
	switch (classType)
	{
		case "server":
			iconClass = icoServer;
			break;
		case "definition":
			iconClass = icoCheck;
			break;
		case "project":
			iconClass = icoProjects;
			break;
		case "datasources":
			iconClass = icoDimensions;
			break;
		case "dimensions":
			iconClass = icoAtomic;
			break;
		case "dimension":
			iconClass = icoCheck;
			break;
		case "cubes":
			iconClass = icoCubes;
			break;
		case "cube":
			iconClass = icoCube;
			break;
		case "logs":
			iconClass = icoLogs;
			break;
		case "log":
			iconClass = icoLog;
			break;
		default:
			iconClass = icoAccept;
			break;
	}
	return iconClass;
}
*/

