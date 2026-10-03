import asyncio, rcmd, time, sys
from meshcore import MeshCore, EventType
async def m():
    rcmd.drain('/dev/ttyACM0')
    mc = await MeshCore.create_serial(port='/dev/ttyACM0', baudrate=115200)
    def rx(e):
        p=e.payload; print(time.strftime('%H:%M:%S'), 'rx', p.get('payload_typename', p.get('payload_type')), 'snr', p.get('snr'), 'path', p.get('path'), 'plen', p.get('path_len'), 'raw', str(p.get('payload',''))[:80], flush=True)
    mc.subscribe(EventType.RX_LOG_DATA, rx)
    mc.subscribe(EventType.ADVERTISEMENT, lambda e: print(time.strftime('%H:%M:%S'),'ADVERT', e.payload, flush=True))
    mc.subscribe(EventType.NEW_CONTACT, lambda e: print(time.strftime('%H:%M:%S'),'NEW_CONTACT', e.payload.get('adv_name'), e.payload.get('public_key','')[:12], e.payload.get('out_path_len'), e.payload.get('out_path'), flush=True))
    mc.subscribe(EventType.PATH_UPDATE, lambda e: print(time.strftime('%H:%M:%S'),'PATH_UPDATE', e.payload, flush=True))
    await asyncio.sleep(float(sys.argv[1]))
    await mc.commands.get_contacts()
    for k,c in mc.contacts.items(): print('CONTACT', c['adv_name'], k[:12], c['out_path_len'], c.get('out_path'))
    await mc.disconnect()
asyncio.run(m())
