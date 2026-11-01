// osascript -l JavaScript export-oc-settings.js <in.plist> <out.plist>
// Copies the Opal settings (keys starting with "OC") from a preferences plist into the file the custom build's
// opalspy imports on first launch. Prints what it did.
ObjC.import('Foundation');
function run(argv) {
    var d = $.NSDictionary.dictionaryWithContentsOfFile(argv[0]);
    if (!d || d.isNil()) return 'none found';
    var out = $.NSMutableDictionary.dictionary;
    var keys = d.allKeys;
    for (var i = 0; i < keys.count; i++) {
        var k = keys.objectAtIndex(i);
        if (k.js.indexOf('OC') === 0) out.setObjectForKey(d.objectForKey(k), k);
    }
    if (out.count === 0) return 'none found';
    return out.writeToFileAtomically(argv[1], true) ? 'exported ' + out.count + ' keys' : 'write failed';
}
