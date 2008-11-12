package util {
    import mx.rpc.IResponder;
    import mx.rpc.AsyncToken;
    import mx.rpc.http.HTTPService;
    import model.GPPerfMonModelLocator;

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
            var call : AsyncToken = service.send();
            if (responder != null) {
                call.addResponder(responder);
            }
        }
    }
}