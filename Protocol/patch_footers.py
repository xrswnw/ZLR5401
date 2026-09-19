#!/usr/bin/env python3
"""Post-process: patch footer PAGE fields with explicit format switches (WPS compat)
and strip empty <w:pgNumType/> from cover section."""
import re, sys, zipfile, shutil, os

path = sys.argv[1]
tmp = path + ".tmp.zip"
zin = zipfile.ZipFile(path, "r")

docxml = zin.read("word/document.xml").decode("utf-8")
rels = zin.read("word/_rels/document.xml.rels").decode("utf-8")

# rId -> footer target
rid2file = dict(re.findall(r'<Relationship[^>]*Id="(rId\d+)"[^>]*Target="(footer\d+\.xml)"', rels))
# also handle attribute order Target before Id
for m in re.finditer(r'<Relationship\b[^>]*>', rels):
    tag = m.group(0)
    rid = re.search(r'Id="(rId\d+)"', tag)
    tgt = re.search(r'Target="(footer\d+\.xml)"', tag)
    if rid and tgt:
        rid2file[rid.group(1)] = tgt.group(1)

# per sectPr: find pgNumType fmt + footerReference
roman_files, arabic_files = set(), set()
for sect in re.finditer(r'<w:sectPr\b.*?</w:sectPr>', docxml, re.S):
    s = sect.group(0)
    fmt = re.search(r'<w:pgNumType[^>]*w:fmt="([^"]+)"', s)
    for fr in re.finditer(r'<w:footerReference[^>]*r:id="(rId\d+)"', s):
        f = rid2file.get(fr.group(1))
        if not f:
            continue
        if fmt and "oman" in fmt.group(1):
            roman_files.add(f)
        else:
            arabic_files.add(f)

print("roman footers:", roman_files, "| arabic footers:", arabic_files)

# strip empty pgNumType (docx-js emits it for sections w/o pageNumbers)
docxml2 = docxml.replace("<w:pgNumType/>", "")

def patch_footer(xml, switch):
    return re.sub(
        r'(<w:instrText[^>]*>)\s*PAGE\s*(</w:instrText>)',
        r'\g<1> PAGE \\* ' + switch + r' \\* MERGEFORMAT \g<2>',
        xml)

zout = zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED)
for item in zin.infolist():
    data = zin.read(item.filename)
    if item.filename == "word/document.xml":
        data = docxml2.encode("utf-8")
    else:
        base = os.path.basename(item.filename)
        if item.filename.startswith("word/footer") and base in roman_files:
            data = patch_footer(data.decode("utf-8"), "ROMAN").encode("utf-8")
        elif item.filename.startswith("word/footer") and base in arabic_files:
            data = patch_footer(data.decode("utf-8"), "arabic").encode("utf-8")
    zout.writestr(item, data)
zout.close(); zin.close()
shutil.move(tmp, path)
print("patched OK:", path)
