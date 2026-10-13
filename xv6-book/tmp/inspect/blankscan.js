ObjC.import('PDFKit');
function run(a){
  var doc=$.PDFDocument.alloc.initWithURL($.NSURL.fileURLWithPath(a[0]));
  if(!doc.js) return "null";
  var lo=parseInt(a[1]), hi=parseInt(a[2]);
  var out=[];
  for(var i=lo-1;i<hi;i++){
    var pg=doc.pageAtIndex(i);
    var s=pg.string;
    var n=s.js?s.js.replace(/\s+/g,'').length:0;
    out.push((i+1)+"\t"+n);
  }
  return out.join("\n");
}
