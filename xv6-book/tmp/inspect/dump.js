ObjC.import('Quartz');
ObjC.import('Foundation');
function run(argv){
  var src = argv[0], outdir = argv[1];
  var doc = $.PDFDocument.alloc.initWithURL($.NSURL.fileURLWithPath(src));
  var n = doc.pageCount;
  var hits = [];
  var keys = ['evdev 队列','内核侧的按键序列','与 Linux 还差什么','DMA 缓冲的所有权',
              '限量日志各自证明什么','每设备状态','引用计数与生命周期','channel 7 怎么保证自己活着'];
  for (var i = 0; i < n; i++){
    var s = doc.pageAtIndex(i).string.js || '';
    for (var j = 0; j < keys.length; j++){
      if (s.indexOf(keys[j]) >= 0) hits.push(keys[j] + ' -> p' + (i+1));
    }
  }
  return 'pages: ' + n + '\n' + hits.join('\n');
}
