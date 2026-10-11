ObjC.import('Quartz');
ObjC.import('Foundation');
// usage: osascript -l JavaScript extract2.js <main.pdf> <outdir> <pageList,comma>
function run(argv) {
  var doc = $.PDFDocument.alloc.initWithURL($.NSURL.fileURLWithPath(argv[0]));
  var out = [];
  argv[2].split(',').forEach(function (n) {
    var page = doc.pageAtIndex(parseInt(n) - 1);
    var p = argv[1] + '/pg' + n + '.pdf';
    // PDFKit methods must be called property-style here: dataRepresentation, not dataRepresentation()
    page.dataRepresentation.writeToFileAtomically(p, true);
    out.push(p);
  });
  return out.join('\n');
}
