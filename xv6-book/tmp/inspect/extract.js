ObjC.import('Quartz');
ObjC.import('Foundation');
function run(argv){
  var src = argv[0], outdir = argv[1];
  var doc = $.PDFDocument.alloc.initWithURL($.NSURL.fileURLWithPath(src));
  var pages = argv[2].split(',');
  var out = $.PDFDocument.alloc.init();
  for (var k = 0; k < pages.length; k++){
    var p = doc.pageAtIndex(parseInt(pages[k]) - 1);
    out.insertPageAtIndex(p, k);
  }
  var path = outdir + '/sel.pdf';
  var ok = out.dataRepresentation().writeToFileAtomically(path, true);
  return 'wrote ' + path + ' ok=' + ok + ' pages=' + out.pageCount;
}
