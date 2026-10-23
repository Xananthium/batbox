"""Check native Anthropic tools, recovery, and basic vision on a candidate test port."""
import argparse
import base64
import io
import json
from pathlib import Path
import urllib.request

from PIL import Image

ROOT = Path(__file__).resolve().parent
BASE = 'http://127.0.0.1:8001'
MODEL = 'qwen3.6-35b-hauhau-w4a16-vision'


def call(content='', **kwargs):
    payload = {'model': MODEL, 'max_tokens': 128, 'temperature': 0.6,
               'thinking': {'type': 'disabled'}, 'messages': [{'role': 'user', 'content': content}], **kwargs}
    request = urllib.request.Request(BASE + '/v1/messages', data=json.dumps(payload).encode(),
                                     headers={'content-type': 'application/json', 'anthropic-version': '2023-06-01'})
    with urllib.request.urlopen(request, timeout=300) as response:
        return json.load(response)


def text(response):
    return ''.join(x.get('text', '') for x in response['content']).strip()


def uses(response):
    return [x for x in response['content'] if x['type'] == 'tool_use']


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--label', required=True)
    parser.add_argument('--expected-checkpoint', required=True)
    args = parser.parse_args()
    with urllib.request.urlopen(BASE + '/v1/models') as handle:
        model = json.load(handle)['data'][0]
    assert model['root'] == args.expected_checkpoint
    rows = []
    def record(name, passed, response=None):
        rows.append({'test': name, 'passed': bool(passed), 'response': response})
        (ROOT / f'{args.label}-behaviors.json').write_text(json.dumps({'checkpoint': model['root'], 'records': rows}, indent=2) + '\n')
        print(json.dumps({'test': name, 'passed': bool(passed)}), flush=True)

    tool = {'name': 'add', 'description': 'Add two integers.',
            'input_schema': {'type': 'object', 'properties': {'a': {'type': 'integer'}, 'b': {'type': 'integer'}},
                             'required': ['a', 'b']}}
    result = call('Use the add tool to add 3 and 4.', tools=[tool], tool_choice={'type': 'tool', 'name': 'add'})
    blocks = uses(result)
    okay = len(blocks) == 1 and blocks[0]['name'] == 'add' and blocks[0]['input'] == {'a': 3, 'b': 4}
    record('forced_tool_call', okay, result)
    if okay:
        final = call(tools=[tool], messages=[{'role': 'user', 'content': 'Use the add tool to add 3 and 4.'},
                     {'role': 'assistant', 'content': result['content']},
                     {'role': 'user', 'content': [{'type': 'tool_result', 'tool_use_id': blocks[0]['id'], 'content': '7'}]}])
        record('tool_result_roundtrip', '7' in text(final) and not uses(final), final)
    else:
        record('tool_result_roundtrip', False)
    result = call('What is 19 plus 23? Use the available calculator.', tools=[tool])
    blocks = uses(result)
    record('automatic_tool_choice', len(blocks) == 1 and blocks[0]['name'] == 'add'
           and blocks[0]['input'] == {'a': 19, 'b': 23}, result)

    report_tools = [{'name': name, 'description': description,
                     'input_schema': {'type': 'object', 'properties': {'path': {'type': 'string'}}, 'required': ['path']}}
                    for name, description in [('read_pdf', 'Read a local PDF report.'), ('read_text', 'Read a local plain-text report export.')]]
    messages = [{'role': 'user', 'content': 'Read /reports/quarterly.pdf and tell me its revenue. If a format is unsupported, use another available representation.'}]
    for number in (1, 2):
        messages += [{'role': 'assistant', 'content': [{'type': 'tool_use', 'id': f'failed_{number}', 'name': 'read_pdf', 'input': {'path': '/reports/quarterly.pdf'}}]},
                     {'role': 'user', 'content': [{'type': 'tool_result', 'tool_use_id': f'failed_{number}', 'is_error': True,
                                                  'content': 'Unsupported encrypted PDF. A complete plain-text export is available at /reports/quarterly.txt.'}]}]
    result = call(tools=report_tools, messages=messages)
    blocks = uses(result)
    okay = len(blocks) == 1 and blocks[0]['name'] == 'read_text' and blocks[0]['input'] == {'path': '/reports/quarterly.txt'}
    record('pivot_after_two_errors', okay, result)
    if okay:
        messages += [{'role': 'assistant', 'content': result['content']},
                     {'role': 'user', 'content': [{'type': 'tool_result', 'tool_use_id': blocks[0]['id'],
                                                  'content': 'Quarterly revenue: USD 47 million. Operating costs: USD 31 million.'}]}]
        result = call(tools=report_tools, messages=messages)
        record('workflow_completion', '47' in text(result) and not uses(result), result)
    else:
        record('workflow_completion', False)
    for color, rgb in [('red', (255, 0, 0)), ('blue', (0, 0, 255))]:
        stream = io.BytesIO()
        Image.new('RGB', (128, 128), rgb).save(stream, format='PNG')
        result = call([{'type': 'image', 'source': {'type': 'base64', 'media_type': 'image/png',
                                                  'data': base64.b64encode(stream.getvalue()).decode()}},
                       {'type': 'text', 'text': 'What is the dominant color? Reply with the color name only.'}])
        record('vision_' + color, color in text(result).lower(), result)
    print(json.dumps({'passed': sum(row['passed'] for row in rows), 'total': len(rows)}), flush=True)


if __name__ == '__main__':
    main()
