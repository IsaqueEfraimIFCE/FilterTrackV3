import os
import subprocess
import re
import pypdf

md_path = r'C:\Users\Isaque\AndroidStudioProjects\FilterTrack\DOCUMENTATION.md'
html_path = r'C:\Users\Isaque\AndroidStudioProjects\FilterTrack\scratch\doc.html'
pdf_path = r'C:\Users\Isaque\AndroidStudioProjects\FilterTrack\DOCUMENTATION.pdf'

os.makedirs(r'C:\Users\Isaque\AndroidStudioProjects\FilterTrack\scratch', exist_ok=True)

with open(md_path, 'r', encoding='utf-8') as f:
    md_content = f.read()

def parse_markdown(md):
    lines = md.splitlines()
    html_out = []
    in_code = False
    code_lang = ''
    code_lines = []
    in_table = False
    table_rows = []
    in_warning = False
    warning_lines = []

    def flush_table():
        nonlocal in_table, table_rows
        if not table_rows:
            return ''
        res = ['<table>']
        header = table_rows[0]
        res.append('<thead><tr>' + ''.join(f'<th>{c.strip()}</th>' for c in header) + '</tr></thead>')
        res.append('<tbody>')
        for row in table_rows[2:]: # skip separator row
            res.append('<tr>' + ''.join(f'<td>{format_inline(c.strip())}</td>' for c in row) + '</tr>')
        res.append('</tbody></table>')
        in_table = False
        table_rows = []
        return '\n'.join(res)

    def flush_warning():
        nonlocal in_warning, warning_lines
        if not warning_lines:
            return ''
        content = '<br/>'.join(format_inline(l) for l in warning_lines)
        res = f'<div class="callout warning"><strong>WARNING:</strong><p>{content}</p></div>'
        in_warning = False
        warning_lines = []
        return res

    def format_inline(text):
        text = text.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
        text = re.sub(r'\*\*(.*?)\*\*', r'<strong>\1</strong>', text)
        text = re.sub(r'\*(.*?)\*', r'<em>\1</em>', text)
        text = re.sub(r'`(.*?)`', r'<code>\1</code>', text)
        text = re.sub(r'\[(.*?)\]\((.*?)\)', r'<a href="\2">\1</a>', text)
        return text

    for line in lines:
        if line.startswith('```'):
            if in_table:
                html_out.append(flush_table())
            if in_warning:
                html_out.append(flush_warning())
            if in_code:
                in_code = False
                code_text = '\n'.join(code_lines)
                html_out.append(f'<pre><code class="{code_lang}">{code_text}</code></pre>')
                code_lines = []
            else:
                in_code = True
                code_lang = line[3:].strip()
            continue

        if in_code:
            code_lines.append(line.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;'))
            continue

        if line.startswith('> [!WARNING]'):
            if in_table:
                html_out.append(flush_table())
            in_warning = True
            warning_lines = []
            continue

        if in_warning:
            if line.startswith('> '):
                warning_lines.append(line[2:].strip())
                continue
            else:
                html_out.append(flush_warning())

        if '|' in line and (line.strip().startswith('|') or line.strip().endswith('|')):
            if not in_table:
                in_table = True
                table_rows = []
            cells = [c for c in line.split('|')[1:-1]]
            table_rows.append(cells)
            continue
        elif in_table:
            html_out.append(flush_table())

        if line.startswith('# '):
            html_out.append(f'<h1>{format_inline(line[2:].strip())}</h1>')
        elif line.startswith('## '):
            html_out.append(f'<h2>{format_inline(line[3:].strip())}</h2>')
        elif line.startswith('### '):
            html_out.append(f'3>{format_inline(line[4:].strip())}</h3>'.replace('3>', '<h3>'))
        elif line.startswith('#### '):
            html_out.append(f'4>{format_inline(line[5:].strip())}</h4>'.replace('4>', '<h4>'))
        elif line.startswith('> '):
            html_out.append(f'<blockquote>{format_inline(line[2:].strip())}</blockquote>')
        elif line.startswith('---'):
            html_out.append('<hr/>')
        elif line.strip() == '':
            html_out.append('<br/>')
        else:
            html_out.append(f'<p>{format_inline(line)}</p>')

    if in_table:
        html_out.append(flush_table())
    if in_warning:
        html_out.append(flush_warning())

    return '\n'.join(html_out)

html_body = parse_markdown(md_content)

full_html = f'''<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>FilterTrack - Master Project Documentation</title>
<style>
  @page {{
    size: A4;
    margin: 20mm 15mm 20mm 15mm;
  }}
  body {{
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    color: #1e293b;
    line-height: 1.5;
    font-size: 10.5pt;
    padding: 0;
    margin: 0;
  }}
  h1 {{
    color: #0f172a;
    font-size: 22pt;
    border-bottom: 2px solid #0284c7;
    padding-bottom: 6px;
    margin-top: 0;
    page-break-before: avoid;
  }}
  h2 {{
    color: #0369a1;
    font-size: 14pt;
    border-bottom: 1px solid #e2e8f0;
    padding-bottom: 4px;
    margin-top: 22px;
    page-break-after: avoid;
  }}
  h3 {{
    color: #0f172a;
    font-size: 12pt;
    margin-top: 16px;
    page-break-after: avoid;
  }}
  h4 {{
    color: #334155;
    font-size: 10.5pt;
    margin-top: 12px;
    page-break-after: avoid;
  }}
  p {{
    margin: 5px 0;
  }}
  blockquote {{
    background: #f1f5f9;
    border-left: 4px solid #0284c7;
    margin: 10px 0;
    padding: 8px 14px;
    color: #334155;
    font-style: italic;
  }}
  .callout.warning {{
    background: #fffbebf8;
    border: 1px solid #fde68a;
    border-left: 5px solid #f59e0b;
    padding: 12px 16px;
    margin: 14px 0;
    border-radius: 4px;
    color: #92400e;
  }}
  table {{
    width: 100%;
    border-collapse: collapse;
    margin: 14px 0;
    font-size: 9pt;
    page-break-inside: avoid;
  }}
  th, td {{
    border: 1px solid #cbd5e1;
    padding: 6px 9px;
    text-align: left;
  }}
  th {{
    background-color: #f8fafc;
    color: #0f172a;
    font-weight: 600;
  }}
  tr:nth-child(even) {{
    background-color: #f8fafc;
  }}
  pre {{
    background: #0f172a;
    color: #f8fafc;
    padding: 12px;
    border-radius: 6px;
    font-family: "Cascadia Code", Consolas, Monaco, monospace;
    font-size: 8.5pt;
    overflow-x: auto;
    white-space: pre-wrap;
    word-break: break-all;
    page-break-inside: avoid;
  }}
  code {{
    font-family: Consolas, Monaco, monospace;
    background: #f1f5f9;
    color: #0f172a;
    padding: 2px 4px;
    border-radius: 3px;
    font-size: 9pt;
  }}
  pre code {{
    background: transparent;
    color: inherit;
    padding: 0;
  }}
  hr {{
    border: none;
    border-top: 1px solid #e2e8f0;
    margin: 18px 0;
  }}
  a {{
    color: #0284c7;
    text-decoration: none;
  }}
</style>
</head>
<body>
{html_body}
</body>
</html>
'''

with open(html_path, 'w', encoding='utf-8') as f:
    f.write(full_html)

edge_bin = r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
cmd = [
    edge_bin,
    '--headless',
    '--disable-gpu',
    '--no-pdf-header-footer',
    f'--print-to-pdf={pdf_path}',
    f'file:///{html_path.replace("\\", "/")}'
]

print('Running Edge headless to render PDF...')
res = subprocess.run(cmd, capture_output=True, text=True)
print('Return code:', res.returncode)

if os.path.exists(pdf_path):
    size_bytes = os.path.getsize(pdf_path)
    print(f'PDF successfully generated: {pdf_path}')
    print(f'File size: {size_bytes} bytes')
    reader = pypdf.PdfReader(pdf_path)
    print(f'Total PDF pages: {len(reader.pages)}')
else:
    print('PDF generation failed:', res.stderr)
