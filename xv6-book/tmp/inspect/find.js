ObjC.import('PDFKit');
function run(a) {
  var path = a[0], term = a[1];
  var doc = $.PDFDocument.alloc.initWithURL($.NSURL.fileURLWithPath(path));
  if (!doc.js) return "null";
  var out = [];
  for (var i=0;i<doc.pageCount;i++){
    var pg = doc.pageAtIndex(i);
    var s = pg.string;
    if (s.js && s.js.indexOf(term)>=0) out.push(i);
  }
  return out.join(",");
}
