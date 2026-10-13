ObjC.import('PDFKit');
function run(a){
  var doc=$.PDFDocument.alloc.initWithURL($.NSURL.fileURLWithPath(a[0]));
  var N=doc.pageCount;
  var out=["TOTAL="+N];
  for(var i=0;i<N;i++){
    var pg=doc.pageAtIndex(i);
    var s=pg.string;
    var n=s.js?s.js.replace(/\s+/g,'').length:0;
    if(n<300) out.push("page "+(i+1)+": "+n+" chars");
  }
  return out.join("\n");
}
