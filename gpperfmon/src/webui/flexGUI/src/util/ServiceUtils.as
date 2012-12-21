package util {
    import model.GPPerfMonModelLocator;
    import mx.rpc.AsyncToken;
    import mx.rpc.IResponder;
    import mx.rpc.events.FaultEvent;
    import mx.rpc.events.ResultEvent;
    import mx.rpc.http.HTTPService;

    public class ServiceUtils {
  		[Bindable]
		public var mdl : GPPerfMonModelLocator = GPPerfMonModelLocator.getInstance();

        public static function send(
            url : String,
            responder : IResponder = null,
            request : Object = null,
            sendXML : Boolean = false,
            resultFormat : String = "e4x",
            method : String = null,
            useProxy : Boolean = false) : void
        {
            var service:HTTPService = new HTTPService();
            service.url = url;
            service.request = request;
            service.contentType = sendXML ? "application/xml" :
                "application/x-www-form-urlencoded";
            service.resultFormat = resultFormat;
            if (method == null) {//the method wasn't set, so default sensibly
                service.method = (request == null) ? "GET" : "POST";
            } else {
                service.method = method;
            }
            service.useProxy = useProxy;
            
            // Dont use call.addResponder(responder) -- calls the handler twice for every call 
            // This is a bug in 3.4 SDK; fixed in 3.5 SDK.
            if (responder != null) {
            	service.addEventListener(ResultEvent.RESULT, responder.result);
				service.addEventListener(FaultEvent.FAULT, responder.fault);
            }

            var call : AsyncToken = service.send();
        }
    }
}