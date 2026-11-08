"""The campaign's shared house and chest (03.10: no guilds — the server is one campaign, everything is shared by all
its players) and marriages — kept on the server (storage world.json), by player name.

Commands come from the consoles as text (SOCIAL packet), the answer is a short text for the menu (SOCIAL_INFO);
events go to everybody as chat lines.
    info
    clan create <name> | clan join <name> | clan leave | clan list
    clan house set <cell> <world> <x> <y> <z>      (hex FormIDs, the place where the member stands)
    marry <player> | accept | divorce
    chest get | chest put <hex id> <n> | chest take <hex id> <n> | chest delta <hex>:<+-n> ...  (the clan's shared chest)
The first line of an answer may be machine data for the menu: "#house <cell> <world> <x> <y> <z>".
"""


class Social:
    def __init__(self, store, online_names, say):
        self.store = store
        self.online = online_names          # () -> list of names now in the room
        self.say = say                      # (text) -> chat to everybody
        w = store.social
        self.clans = w.setdefault('clans', {})          # (old: guilds) merged into the campaign once
        self.campaign = w.setdefault('campaign', {'house': None, 'chest': {}})
        for c in list(self.clans.values()):               # the guilds of before: their chests and a house go to the campaign
            for k, v in c.get('chest', {}).items():
                self.campaign['chest'][k] = self.campaign['chest'].get(k, 0) + v
            for k, v in c.get('quests', {}).items():      # and their quest stages (the highest wins)
                store.quests[int(k)] = max(int(v), int(store.quests.get(int(k), -1)))
            if c.get('house') and not self.campaign.get('house'):
                self.campaign['house'] = c['house']
        if self.clans:
            self.clans.clear()
            store.save_world()
        self.married = w.setdefault('married', {})      # name -> partner
        self.proposals = {}                             # proposed-to -> proposer (this session)
        self.on_join = None                             # (player name) -> called after joining a clan

    def clan_of(self, who):
        """Everybody is in the one campaign."""
        return 'кампания', self.campaign

    def save(self):
        self.store.save_world()

    def info(self, who):
        lines = []
        h = self.campaign.get('house')
        if h:
            lines.append('#house %s %s %.0f %.0f %.0f' % (h['cell'], h['world'], h['x'], h['y'], h['z']))
        lines.append('Кампания: всё общее — сюжет, мир, сундук и дом.')
        lines.append('В игре: ' + (', '.join(self.online()) or '—'))
        lines.append('Дом кампании: ' + ('есть' if h else 'нет — любой может назначить его там, где стоит'))
        if who in self.married:
            lines.append('Супруг(а): ' + self.married[who])
        if who in self.proposals:
            lines.append('%s предлагает тебе брак — «Принять предложение»' % self.proposals[who])
        return '\n'.join(lines)

    def run(self, who, line):
        a = line.strip().split()
        if not a or a[0] == 'info':
            return self.info(who)
        if a and a[0] in ('guild', 'гильдия'):           # the new name of the same thing (03.10)
            a[0] = 'clan'
        if a[0] == 'clan' and len(a) >= 2:
            if a[1] == 'house' and len(a) >= 8 and a[2] == 'set':
                self.campaign['house'] = {'cell': a[3], 'world': a[4], 'x': float(a[5]), 'y': float(a[6]), 'z': float(a[7])}
                self.save()
                self.say('%s назначил(а) дом кампании' % who)
                return self.info(who)
            if a[1] in ('create', 'join', 'leave'):
                return 'Гильдий нет: сервер — одна кампания, всё общее для всех игроков.'
            return self.info(who)
        if a[0] == 'chest' and len(a) >= 2:
            chest = self.campaign.setdefault('chest', {})                 # hex FormID -> count
            if a[1] == 'get':
                return '#chest ' + ' '.join('%s:%d' % (k, v) for k, v in sorted(chest.items()) if v > 0)
            if a[1] == 'delta':                               # several changes at once: <hex>:<+n|-n> ...
                denied = []
                for tok in a[2:]:
                    fid, _, n = tok.partition(':')
                    fid, n = '%08X' % int(fid, 16), int(n)
                    have = chest.get(fid, 0)
                    if n >= 0:
                        chest[fid] = have + min(n, 1000000)
                    else:
                        got = min(have, -n)
                        chest[fid] = have - got
                        if got < -n:
                            denied.append('%s:%d' % (fid, -n - got))
                    if not chest.get(fid):
                        chest.pop(fid, None)
                self.save()
                return '#chestdeny ' + ' '.join(denied) if denied else '#chestok'
            if a[1] in ('put', 'take') and len(a) >= 4:
                fid = '%08X' % int(a[2], 16)
                n = max(0, min(int(a[3]), 1000000))
                if a[1] == 'put':
                    chest[fid] = chest.get(fid, 0) + n
                    self.save()
                    return '#chestok'
                have = chest.get(fid, 0)
                got = min(have, n)
                if got:
                    chest[fid] = have - got
                    if not chest[fid]:
                        del chest[fid]
                    self.save()
                return '#chestok' if got == n else '#chestdeny %s %d' % (fid, n - got)
        if a[0] == 'marry' and len(a) >= 2:
            target = ' '.join(a[1:])
            if target == who:
                return 'Нельзя.'
            if who in self.married:
                return 'Ты уже в браке с %s.' % self.married[who]
            if target not in self.online():
                return '%s сейчас не в комнате.' % target
            self.proposals[target] = who
            self.say('%s делает предложение %s!' % (who, target))
            return 'Предложение отправлено.'
        if a[0] == 'accept':
            other = self.proposals.pop(who, None)
            if not other:
                return 'Предложений нет.'
            if other in self.married or who in self.married:
                return 'Кто-то из вас уже в браке.'
            self.married[who] = other
            self.married[other] = who
            self.save()
            self.say('%s и %s поженились! Поздравляем!' % (other, who))
            return self.info(who)
        if a[0] == 'divorce':
            other = self.married.pop(who, None)
            if not other:
                return 'Ты не в браке.'
            self.married.pop(other, None)
            self.save()
            self.say('%s и %s развелись' % (who, other))
            return self.info(who)
        return 'Непонятная команда.'
