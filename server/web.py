"""Web panel of the server: http://<pc>:8080 from any browser in the network (PC, phone).

JSON API (polled by web/index.html once a second):
    GET  /api/state            room, addresses, players, uptime, settings, world
    GET  /api/log?after=N      log lines after sequence N
    GET  /api/chat             last chat messages
    POST /api/say              {"text", "to"?}           message to everybody / one player
    POST /api/kick             {"who"}                   kick (and ban by name)
    POST /api/unban            {"name"} or {}            unban one / all
    POST /api/cmd              {"who", "cmd"}            console command on that console (admin)
    POST /api/settings         {"name"?, "time_mode"?, "fixed_hour"?, "web_password"?}
    POST /api/revive           {"formId"?}               forget one / all kills of the shared world
    POST /api/shutdown         {}                        save the world and stop the server
    POST /api/reset            {}                        a new campaign: the old world, campaign saves and avatars go
                                                         to old_campaigns/<time>, then the server stops
Optional password (settings web_password): HTTP Basic, any user name.
"""
import asyncio, base64, json, os, shutil, sys, time
from aiohttp import web
from protocol import CMD, STATS_S
from coop_server import local_addresses
import updates

ROOT = getattr(sys, '_MEIPASS', os.path.dirname(os.path.abspath(__file__)))
PAGE = os.path.join(ROOT, 'web', 'index.html')


def make_app(srv):
    store = srv.store

    @web.middleware
    async def auth(request, handler):
        pw = store.settings.get('web_password')
        if pw:
            h = request.headers.get('Authorization', '')
            ok = False
            if h.startswith('Basic '):
                try:
                    ok = base64.b64decode(h[6:]).decode('utf-8').split(':', 1)[1] == pw
                except Exception:  # noqa: BLE001
                    ok = False
            if not ok:
                return web.Response(status=401, headers={'WWW-Authenticate': 'Basic realm="SkyrimCoop"'})
        return await handler(request)

    async def page(_):
        return web.FileResponse(PAGE, headers={'Cache-Control': 'no-cache'})

    async def state(_):
        host = min(srv.players) if srv.players else None
        players = [{
            'id': p.id, 'name': p.name, 'addr': p.addr, 'place': p.place(),
            'minutes': int((time.time() - p.joined) // 60), 'host': p.id == host,
            'look': bool(p.appearance), 'items': p.equipment[0] if p.equipment else 0,
            'mods': len(p.plugins) if p.plugins else None,
            **(dict(zip(('level', 'hp', 'mp', 'sp'), STATS_S.unpack(p.stats)[1:5])) if p.stats else {}),
        } for p in srv.players.values()]
        st = store.settings
        return web.json_response({
            'room': srv.name, 'port': srv.port, 'addresses': local_addresses(), 'online': srv.tcp is not None,
            'uptime': int(time.time() - srv.started), 'players': players,
            'settings': {k: st.get(k) for k in ('name', 'time_mode', 'fixed_hour', 'web_port', 'pvp', 'shared_quests')},
            'has_password': bool(st.get('web_password')),
            'campaign': {'id': store.campaign_id, 'quests': len(store.quests)},
            'world': {'killed': ['%08X' % f for f in srv.killed], 'banned': sorted(srv.banned),
                      'clans': {n: {'leader': c['leader'], 'members': c['members'], 'house': bool(c.get('house')),
                                    'chest': sum(c.get('chest', {}).values()), 'quests': len(c.get('quests', {}))}
                                for n, c in srv.social.clans.items()},
                      'married': sorted({tuple(sorted(x)) for x in srv.social.married.items()})},
            'zones': [{'name': srv.zone_name(z), 'owner': srv.players[o].name if o in srv.players else '?',
                       'players': [p.name for p in srv.players.values() if p.zone == z]}
                      for z, o in srv.zone_owner.items()],
            'updates': {'folder': updates.folder(store),
                        'files': [{'path': r, 'size': n} for r, n, _ in updates.manifest(store)][:200]},
        })

    async def log(request):
        after = int(request.query.get('after', 0))
        return web.json_response({'lines': [[n, t] for n, t in srv.lines if n > after], 'last': srv.seq})

    async def chat(_):
        return web.json_response([[time.strftime('%H:%M', time.localtime(t)), w, x] for t, w, x in srv.chat])

    async def body(request):
        try:
            return await request.json()
        except Exception:  # noqa: BLE001
            return {}

    async def say(request):
        b = await body(request)
        text = str(b.get('text', '')).strip()[:190]
        n = srv.say(text, b.get('to') or None) if text else 0
        return web.json_response({'sent': n})

    async def kick(request):
        b = await body(request)
        return web.json_response({'kicked': srv.kick(str(b.get('who', '')))})

    async def unban(request):
        b = await body(request)
        if b.get('name'):
            srv.unban(str(b['name']))
        else:
            srv.unban_all()
        return web.json_response({'ok': True})

    async def cmd(request):
        b = await body(request)
        text = str(b.get('cmd', '')).strip()[:199]
        ps = srv.targets(str(b.get('who', ''))) if text else []
        for p in ps:
            srv.send(p, CMD, text.encode('utf-8'))
        srv.log('[%s] панель: команда «%s» -> %d' % (srv.stamp(), text, len(ps)))
        return web.json_response({'sent': len(ps)})

    async def settings(request):
        b = await body(request)
        st = store.settings
        if 'name' in b:
            st['name'] = srv.name = str(b['name']).strip()[:40]
        if b.get('time_mode') in ('host', 'fixed'):
            st['time_mode'] = b['time_mode']
        if 'fixed_hour' in b:
            try:
                st['fixed_hour'] = max(0.0, min(23.99, float(b['fixed_hour'])))
            except (TypeError, ValueError):
                pass
        if 'pvp' in b:
            st['pvp'] = bool(b['pvp'])
        if 'shared_quests' in b:
            st['shared_quests'] = bool(b['shared_quests'])
        if 'web_password' in b:
            st['web_password'] = str(b['web_password'])[:64]
        store.save_settings()
        srv.log('[%s] панель: настройки изменены' % srv.stamp())
        return web.json_response({'ok': True})

    async def revive(request):
        b = await body(request)
        srv.revive(int(b['formId'], 16) if b.get('formId') else None)
        return web.json_response({'ok': True})

    def stop_later(fn):
        """answer the page first, then stop (the exe window closes)"""
        def go():
            try:
                fn()
            finally:
                os._exit(0)
        asyncio.get_event_loop().call_later(0.5, go)

    async def shutdown(_):
        srv.log('[%s] панель: сервер выключен' % srv.stamp())
        stop_later(store.save_world)
        return web.json_response({'ok': True})

    async def reset(_):
        """(07.10) a new campaign from zero: nothing of the old one is deleted, it is moved aside"""
        srv.log('[%s] панель: кампания сброшена, сервер выключается' % srv.stamp())

        def move():
            dst = os.path.join(store.dir, 'old_campaigns', time.strftime('%Y%m%d_%H%M%S'))
            os.makedirs(dst, exist_ok=True)
            with store.lock:
                for name in ('world.json', 'saves', 'avatars'):
                    src = os.path.join(store.dir, name)
                    if os.path.exists(src):
                        shutil.move(src, os.path.join(dst, name))
            # the ban list is the room's, not the campaign's: kept
            if store.banned:
                with open(os.path.join(store.dir, 'world.json'), 'w', encoding='utf-8') as f:
                    json.dump({'banned': sorted(store.banned)}, f, ensure_ascii=False)
        stop_later(move)
        return web.json_response({'ok': True})

    app = web.Application(middlewares=[auth])
    app.router.add_get('/', page)
    app.router.add_get('/api/state', state)
    app.router.add_get('/api/log', log)
    app.router.add_get('/api/chat', chat)
    for path, fn in (('say', say), ('kick', kick), ('unban', unban), ('cmd', cmd), ('settings', settings), ('revive', revive),
                     ('shutdown', shutdown), ('reset', reset)):
        app.router.add_post('/api/' + path, fn)
    return app


async def start_web(srv, port):
    runner = web.AppRunner(make_app(srv), access_log=None)
    await runner.setup()
    await web.TCPSite(runner, '0.0.0.0', port).start()
    return runner
