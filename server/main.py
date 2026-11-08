"""SkyrimCoop server with its web panel — what SkyrimCoopServer.exe and the Docker image run.

    python main.py [--name Комната] [--port 27015] [--web-port 8080] [--no-browser] [--quiet]
Settings given here are saved (see storage.py) and used next time.
"""
import argparse, asyncio, os, socket, sys, webbrowser
from coop_server import Server, local_addresses
from storage import Store
from web import start_web


async def run(a):
    store = Store()
    st = store.settings
    if a.name:
        st['name'] = a.name
    if a.port:
        st['port'] = a.port
    if a.web_port:
        st['web_port'] = a.web_port
    if st['name'] == 'Skyrim' and not a.name:        # no name: consoles show the server's address
        st['name'] = os.environ.get('SKYCOOP_ROOM', '')
    store.save_settings()
    srv = Server(st['name'], st['port'], log=print, verbose=not a.quiet, store=store)
    await srv.start()
    await start_web(srv, st['web_port'])
    url = 'http://localhost:%d' % st['web_port']
    print('Панель управления: %s  (с телефона/другого ПК: %s)' % (
        url, ', '.join('http://%s:%d' % (ip, st['web_port']) for ip in local_addresses())))
    print('Закрой это окно, чтобы остановить сервер.')
    if not a.no_browser:
        webbrowser.open(url)
    await asyncio.Event().wait()


if __name__ == '__main__':
    try:
        sys.stdout.reconfigure(encoding='utf-8', line_buffering=True)
    except AttributeError:
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument('--name')
    ap.add_argument('--port', type=int)
    ap.add_argument('--web-port', type=int)
    ap.add_argument('--no-browser', action='store_true')
    ap.add_argument('--quiet', action='store_true', help='do not print positions every 2 s')
    try:
        asyncio.run(run(ap.parse_args()))
    except KeyboardInterrupt:
        pass
