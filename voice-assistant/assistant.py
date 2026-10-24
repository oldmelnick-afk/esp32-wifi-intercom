"""Voice assistant core: speech -> text -> GigaChat with tools -> speech.

Audio everywhere is the intercom format: 16 kHz, 16-bit, mono PCM.
"""
import asyncio
import io
import json
import re
import time
import uuid
from datetime import datetime, timedelta
from pathlib import Path

import numpy as np
import requests
import truststore

truststore.inject_into_ssl()  # trust the Windows store (Russian root CA for Sber)

HERE = Path(__file__).resolve().parent
CONFIG = json.loads((HERE / "config.json").read_text(encoding="utf-8"))
SAMPLE_RATE = 16000

SYSTEM_PROMPT = (
    "Ты голосовой помощник домашней переговорки. Отвечай по-русски, коротко и "
    "разговорно: одна-две фразы, без списков, markdown и эмодзи, потому что ответ "
    "будет озвучен. Числа и единицы пиши словами там, где так естественнее "
    "произносить. Для погоды, таймера, времени и дороги вызывай функции, не выдумывай. "
    f"Дом находится в городе {CONFIG['home']['name']}."
)

TOOLS = [
    {"name": "get_weather",
     "description": "Текущая погода и прогноз на сегодня и завтра в городе (по умолчанию дома).",
     "parameters": {"type": "object", "properties": {
         "city": {"type": "string", "description": "Город; пусто = дом"}}}},
    {"name": "set_timer",
     "description": "Поставить таймер: по истечении зазвучит мелодия. По умолчанию только "
                    "на коробке, с которой спрашивают.",
     "parameters": {"type": "object", "properties": {
         "seconds": {"type": "integer", "description": "Длительность в секундах"},
         "all_boxes": {"type": "boolean",
                       "description": "true только если явно просят на всех коробках / всем"}},
         "required": ["seconds"]}},
    {"name": "stop_timer",
     "description": "Отменить таймер или выключить звучащую мелодию (по умолчанию на этой коробке).",
     "parameters": {"type": "object", "properties": {
         "all_boxes": {"type": "boolean",
                       "description": "true только если просят остановить везде / всем"}}}},
    {"name": "get_time",
     "description": "Текущие дата, день недели и время.",
     "parameters": {"type": "object", "properties": {}}},
    {"name": "get_route",
     "description": "Расстояние и время в пути на машине от дома до места (без учёта пробок).",
     "parameters": {"type": "object", "properties": {
         "destination": {"type": "string", "description": "Город или адрес назначения"}},
         "required": ["destination"]}},
]

# ---------------------------------------------------------------- GigaChat

class GigaChat:
    def __init__(self, key_file):
        lines = [l.strip() for l in Path(key_file).read_text(encoding="utf-8-sig").splitlines()
                 if l.strip()]
        self.scope, self.auth_key = lines[1], lines[2]
        self.token, self.expires = None, 0

    def _token(self):
        if self.token and time.time() * 1000 < self.expires - 60_000:
            return self.token
        r = requests.post("https://ngw.devices.sberbank.ru:9443/api/v2/oauth",
                          headers={"Authorization": f"Basic {self.auth_key}",
                                   "RqUID": str(uuid.uuid4()),
                                   "Accept": "application/json"},
                          data={"scope": self.scope}, timeout=20)
        r.raise_for_status()
        data = r.json()
        self.token, self.expires = data["access_token"], data["expires_at"]
        return self.token

    def chat(self, messages, functions=None):
        body = {"model": CONFIG["gigachat_model"], "messages": messages, "temperature": 0.3}
        if functions:
            body["functions"] = functions
            body["function_call"] = "auto"
        r = requests.post("https://gigachat.devices.sberbank.ru/api/v1/chat/completions",
                          headers={"Authorization": f"Bearer {self._token()}"},
                          json=body, timeout=40)
        r.raise_for_status()
        return r.json()["choices"][0]["message"]

# ---------------------------------------------------------------- tools

WEATHER_CODES = {0: "ясно", 1: "в основном ясно", 2: "переменная облачность", 3: "пасмурно",
                 45: "туман", 48: "изморозь", 51: "морось", 53: "морось", 55: "сильная морось",
                 61: "небольшой дождь", 63: "дождь", 65: "сильный дождь", 66: "ледяной дождь",
                 67: "ледяной дождь", 71: "небольшой снег", 73: "снег", 75: "сильный снег",
                 77: "снежная крупа", 80: "ливень", 81: "ливни", 82: "сильные ливни",
                 85: "снегопад", 86: "сильный снегопад", 95: "гроза", 96: "гроза с градом",
                 99: "гроза с градом"}

def web_get(url, **kwargs):
    """GET for the public web services (weather, maps). A short connect timeout:
    if the route is dead (e.g. a VPN on the server swallowing foreign traffic)
    the answer comes quickly instead of after 15 s of silence."""
    kwargs.setdefault("headers", {"User-Agent": "home-intercom-assistant/1.0"})
    r = requests.get(url, timeout=(5, kwargs.pop("timeout", 15)), **kwargs)
    r.raise_for_status()
    return r

def geocode(place):
    r = web_get("https://nominatim.openstreetmap.org/search",
                params={"q": place, "format": "json", "limit": 1, "accept-language": "ru"})
    r.raise_for_status()
    hits = r.json()
    if not hits:
        raise ValueError(f"не нашёл место «{place}»")
    return float(hits[0]["lat"]), float(hits[0]["lon"]), hits[0]["display_name"].split(",")[0]

_weather_cache = {}  # place -> (time, result); the forecast changes slowly
WEATHER_FILE = HERE / "weather-cache.json"   # last good home forecast, survives restarts
WEATHER_STALE_S = 6 * 3600                   # an old forecast beats no answer, up to this age
_weather_failed_at = 0.0

def get_weather(city=""):
    key = city.strip().lower()
    cached = _weather_cache.get(key)
    if not cached and not key and WEATHER_FILE.exists():
        try:
            saved = json.loads(WEATHER_FILE.read_text(encoding="utf-8"))
            cached = _weather_cache[key] = (saved["time"], saved["weather"])
        except Exception:
            pass
    if cached and time.time() - cached[0] < (1800 if not key else 600):
        return cached[1]   # home forecast is kept fresh by the gateway every 9 min
    global _weather_failed_at
    usable = cached and time.time() - cached[0] < WEATHER_STALE_S
    if usable and time.time() - _weather_failed_at < 300:
        return cached[1]   # the service just failed: do not make the user wait again
    try:
        result = _fetch_weather(city)
    except Exception:
        _weather_failed_at = time.time()
        if usable:
            print("weather service down, using the forecast from",
                  time.strftime("%H:%M", time.localtime(cached[0])), flush=True)
            return cached[1]
        raise
    _weather_cache[key] = (time.time(), result)
    if not key:
        WEATHER_FILE.write_text(json.dumps({"time": time.time(), "weather": result},
                                           ensure_ascii=False), encoding="utf-8")
    return result

_open_meteo_down_until = 0.0   # skip Open-Meteo for a while after it failed

def _fetch_weather(city):
    """Open-Meteo, or the Norwegian met.no when it is unreachable (from the home
    network Open-Meteo's Hetzner servers time out without a VPN)."""
    global _open_meteo_down_until
    if city:
        lat, lon, name = geocode(city)
    else:
        lat, lon, name = CONFIG["home"]["lat"], CONFIG["home"]["lon"], CONFIG["home"]["name"]
    if time.time() > _open_meteo_down_until:
        try:
            return _fetch_open_meteo(lat, lon, name)
        except Exception as error:
            print("open-meteo failed, using met.no:", type(error).__name__, flush=True)
            _open_meteo_down_until = time.time() + 3600
    return _fetch_met_no(lat, lon, name)

MET_NO_WEATHER = {"clearsky": "ясно", "fair": "малооблачно", "partlycloudy": "переменная облачность",
                  "cloudy": "пасмурно", "fog": "туман", "lightrain": "небольшой дождь",
                  "rain": "дождь", "heavyrain": "сильный дождь",
                  "lightrainshowers": "небольшой дождь", "rainshowers": "ливни",
                  "heavyrainshowers": "сильные ливни", "lightsleet": "мокрый снег",
                  "sleet": "мокрый снег", "heavysleet": "мокрый снег",
                  "lightsleetshowers": "мокрый снег", "sleetshowers": "мокрый снег",
                  "heavysleetshowers": "мокрый снег", "lightsnow": "небольшой снег",
                  "snow": "снег", "heavysnow": "сильный снег", "lightsnowshowers": "небольшой снег",
                  "snowshowers": "снегопад", "heavysnowshowers": "сильный снегопад"}

def _met_no_words(symbol):
    code = symbol.split("_")[0]
    if "thunder" in code:
        return "гроза"
    return MET_NO_WEATHER.get(code, "без осадков")

def _fetch_met_no(lat, lon, name):
    series = web_get("https://api.met.no/weatherapi/locationforecast/2.0/complete",
                     params={"lat": round(lat, 3), "lon": round(lon, 3)},
                     headers={"User-Agent": "home-intercom-assistant/1.0 (private home use)"}
                     ).json()["properties"]["timeseries"]
    now = datetime.now().astimezone()
    points = []   # (local time, temperature, data)
    for entry in series:
        when = datetime.fromisoformat(entry["time"].replace("Z", "+00:00")).astimezone()
        points.append((when, entry["data"]["instant"]["details"]["air_temperature"], entry["data"]))
    current = min(points, key=lambda p: abs((p[0] - now).total_seconds()))
    details = current[2]["instant"]["details"]
    summary = lambda data, *periods: next(
        (data[p]["summary"]["symbol_code"] for p in periods if p in data), "")

    def day(date, noon_hour=13):
        hours = [p for p in points if p[0].date() == date]
        temps = [t for _, t, _ in hours] or [current[1]]
        for _, _, data in hours:   # 6-hour steps also carry the min/max between them
            six = data.get("next_6_hours", {}).get("details", {})
            temps += [six[k] for k in ("air_temperature_min", "air_temperature_max") if k in six]
        # hourly steps carry 1-hour amounts, the later 6-hour steps 6-hour amounts
        rain = sum(d.get("next_1_hours", d.get("next_6_hours", {})).get("details", {})
                   .get("precipitation_amount", 0) for _, _, d in hours)
        noon = min(hours, key=lambda p: abs(p[0].hour - noon_hour)) if hours else current
        return {"мин": round(min(temps)), "макс": round(max(temps)),
                "погода": _met_no_words(summary(noon[2], "next_6_hours", "next_12_hours")),
                # met.no gives no probability outside Scandinavia: a rough one from the amount
                "вероятность_осадков": 70 if rain >= 1 else 40 if rain >= 0.3 else 0}

    next_12 = [t for when, t, _ in points if now <= when <= now.replace(microsecond=0) +
               timedelta(hours=12)] or [details["air_temperature"]]
    tomorrow = (now + timedelta(days=1)).date()
    return {"место": name,
            "ночью": round(min(next_12)),
            "сейчас": {"температура": round(details["air_temperature"]),
                       "ощущается": round(details.get("apparent_air_temperature",
                                                      details["air_temperature"])),
                       "погода": _met_no_words(summary(current[2], "next_1_hours", "next_6_hours")),
                       "ветер_м_с": round(details.get("wind_speed", 0))},
            "сегодня": day(now.date()),
            "завтра": day(tomorrow)}

def _fetch_open_meteo(lat, lon, name):
    d = web_get("https://api.open-meteo.com/v1/forecast", params={
        "latitude": lat, "longitude": lon, "timezone": "auto", "forecast_days": 2,
        "current": "temperature_2m,apparent_temperature,weather_code,wind_speed_10m",
        "daily": "temperature_2m_max,temperature_2m_min,weather_code,precipitation_probability_max",
        "hourly": "temperature_2m"}).json()
    cur, day = d["current"], d["daily"]
    describe = lambda code: WEATHER_CODES.get(code, "без осадков")
    # coldest of the next 12 hours: "ночью" in evening answers
    now_hour = cur["time"][:13]
    hours = d["hourly"]["time"]
    start = next((i for i, h in enumerate(hours) if h[:13] >= now_hour), 0)
    next_12 = d["hourly"]["temperature_2m"][start:start + 12] or [cur["temperature_2m"]]
    return {"место": name,
            "ночью": round(min(next_12)),
            "сейчас": {"температура": round(cur["temperature_2m"]),
                       "ощущается": round(cur["apparent_temperature"]),
                       "погода": describe(cur["weather_code"]),
                       "ветер_м_с": round(cur["wind_speed_10m"] / 3.6)},
            "сегодня": {"мин": round(day["temperature_2m_min"][0]),
                        "макс": round(day["temperature_2m_max"][0]),
                        "погода": describe(day["weather_code"][0]),
                        "вероятность_осадков": day["precipitation_probability_max"][0]},
            "завтра": {"мин": round(day["temperature_2m_min"][1]),
                       "макс": round(day["temperature_2m_max"][1]),
                       "погода": describe(day["weather_code"][1]),
                       "вероятность_осадков": day["precipitation_probability_max"][1]}}

CURRENT_BOX = None   # IP of the box that asked the current question (set by Assistant)

class BoxError(Exception):
    """The intercom box did not take the command (not GigaChat's fault)."""

def box_command(command):
    box = CURRENT_BOX or CONFIG["control_box"]
    try:
        r = requests.get(f"http://{box}/cmd",
                         params={"code": CONFIG["control_code"], "c": command}, timeout=5)
    except requests.RequestException as error:
        raise BoxError(f"коробка {box} не отвечает") from error
    return r.text.strip()

def set_timer(seconds, all_boxes=False):
    """By default the timer rings only on the box that asked; all_boxes = everywhere."""
    seconds = int(seconds)
    if not 1 <= seconds <= 86400:
        return {"ошибка": "таймер от 1 секунды до 24 часов"}
    command = f"TIMER {seconds}" if all_boxes else f"TIMER_LOCAL {seconds}"
    return {"ответ_коробки": box_command(command), "секунд": seconds,
            "где": "на всех коробках" if all_boxes else "на этой коробке"}

def stop_timer(all_boxes=False):
    return {"ответ_коробки": box_command("STOP" if all_boxes else "STOP_LOCAL")}

def get_time():
    days = ["понедельник", "вторник", "среда", "четверг", "пятница", "суббота", "воскресенье"]
    now = datetime.now()
    return {"дата": now.strftime("%d.%m.%Y"), "день_недели": days[now.weekday()],
            "время": now.strftime("%H:%M")}

def get_route(destination):
    lat2, lon2, name = geocode(destination)
    lat1, lon1 = CONFIG["home"]["lat"], CONFIG["home"]["lon"]
    r = web_get(f"https://router.project-osrm.org/route/v1/driving/"
                f"{lon1},{lat1};{lon2},{lat2}", params={"overview": "false"}, timeout=20)
    route = r.json()["routes"][0]
    minutes = round(route["duration"] / 60)
    return {"куда": name, "км": round(route["distance"] / 1000),
            "время_без_пробок": f"{minutes // 60} ч {minutes % 60} мин"}

TOOL_FUNCS = {"get_weather": get_weather, "set_timer": set_timer, "stop_timer": stop_timer,
              "get_time": get_time, "get_route": get_route}

# ---------------------------------------------------------------- speech

_whisper = None

def transcribe(pcm16):
    """pcm16: numpy int16 array at 16 kHz -> recognised Russian text."""
    global _whisper
    if _whisper is None:
        from faster_whisper import WhisperModel
        local = HERE / CONFIG["whisper_model"]
        model = str(local) if local.exists() else CONFIG["whisper_model"]
        try:
            # The GPU needs CUDA libraries; errors may only appear on first use,
            # so probe with a short silent clip before trusting it.
            _whisper = WhisperModel(model, device="cuda", compute_type="float16")
            list(_whisper.transcribe(np.zeros(SAMPLE_RATE, np.float32), language="ru")[0])
        except Exception:
            # Default threads on purpose: all cores made the voice (torch) that
            # runs right after it 10x slower on the server.
            _whisper = WhisperModel(model, device="cpu", compute_type="int8")
    audio = pcm16.astype(np.float32) / 32768.0
    # One greedy pass: no re-decoding at higher temperatures when unsure (that
    # doubled the time on some questions) and no timestamps, which we never use.
    segments, _ = _whisper.transcribe(audio, language="ru", beam_size=1, vad_filter=True,
                                      temperature=0.0, without_timestamps=True,
                                      condition_on_previous_text=False)
    return " ".join(s.text.strip() for s in segments).strip()

def sentences(text):
    """Split an answer into short phrases so the first can play while the rest synthesize."""
    import re
    parts = [p.strip() for p in re.split(r"(?<=[.!?…])\s+", text) if p.strip()]
    return parts or [text]

_silero = None

MONTHS_GEN = ("января", "февраля", "марта", "апреля", "мая", "июня", "июля", "августа",
              "сентября", "октября", "ноября", "декабря")
FEMININE = ("минут", "секунд", "недел", "тысяч", "копе", "штук", "част", "сотн", "единиц")
UNITS = {"ч": ("час", "часа", "часов"), "мин": ("минута", "минуты", "минут"),
         "сек": ("секунда", "секунды", "секунд"), "км": ("километр", "километра", "километров"),
         "м": ("метр", "метра", "метров"), "°c": ("градус", "градуса", "градусов"),
         "°": ("градус", "градуса", "градусов"), "%": ("процент", "процента", "процентов"),
         "кг": ("килограмм", "килограмма", "килограммов"), "руб": ("рубль", "рубля", "рублей"),
         "₽": ("рубль", "рубля", "рублей")}

def _cardinal(n, feminine=False):
    from num2words import num2words
    words = num2words(n, lang="ru")
    if feminine:
        words = re.sub(r"\bодин$", "одна", words)
        words = re.sub(r"\bдва$", "две", words)
    return words

def _ordinal_neuter(n):
    """21 -> 'двадцать первое' (for dates: 'двадцать первое октября')."""
    from num2words import num2words
    words = num2words(n, lang="ru", to="ordinal")
    last = words.split()[-1]
    for old, new in (("третий", "третье"), ("ый", "ое"), ("ой", "ое"), ("ий", "ее")):
        if last.endswith(old):
            last = last[: -len(old)] + new
            break
    return " ".join(words.split()[:-1] + [last])

def speakable(text):
    """Digits and symbols -> Russian words: Silero skips anything that is not a word."""
    from num2words import num2words
    t = text.replace("−", "-")
    # time 22:05 -> двадцать два ноль пять
    t = re.sub(r"\b(\d{1,2}):(\d{2})\b", lambda m: _cardinal(int(m.group(1))) + " " +
               ("ноль " + _cardinal(int(m.group(2))) if m.group(2).startswith("0") and
                m.group(2) != "00" else "ровно" if m.group(2) == "00" else
                _cardinal(int(m.group(2)), feminine=True)), t)
    # dates: 21 октября -> двадцать первое октября
    t = re.sub(r"\b(\d{1,2})\s+(" + "|".join(MONTHS_GEN) + r")\b",
               lambda m: _ordinal_neuter(int(m.group(1))) + " " + m.group(2), t)
    # number + unit: 2 ч -> два часа, 15% -> пятнадцать процентов, 10 км -> десять километров
    def unit(m):
        n, u = int(m.group(1)), m.group(2).rstrip(".").lower()
        return f"{_cardinal(n, feminine=u in ('мин', 'сек'))} {plural(n, *UNITS[u])}"
    t = re.sub(r"(-?\d+)\s*(°C|°c|°|%|₽|(?:ч|мин|сек|км|кг|руб|м)\b\.?)", unit, t)
    # decimals 2.5 / 2,5
    t = re.sub(r"(-?\d+)[.,](\d+)", lambda m: num2words(float(f"{m.group(1)}.{m.group(2)}"),
                                                        lang="ru"), t)
    # plain integers; feminine before minutes, seconds, weeks, thousands...
    def integer(m):
        n = int(m.group(1))
        following = m.group(2).strip().lower()
        return _cardinal(n, feminine=following.startswith(FEMININE)) + m.group(2)
    t = re.sub(r"(-?\d+)(\s*[А-Яа-яЁё]*)", integer, t)
    return t.replace("-", " ").replace("  ", " ").strip()

def synthesize(text):
    """Russian text -> numpy int16 array at 16 kHz (Silero locally, or Edge TTS)."""
    if CONFIG.get("tts_engine", "silero") == "silero":
        try:
            return _synthesize_silero(speakable(text))
        except Exception as error:   # never leave a question unanswered
            print("silero failed, using Edge TTS:", error, flush=True)
    return _synthesize_edge(text)

def _synthesize_silero(text):
    """Local Silero v4 voice: ~0.1 s per phrase on the CPU, no internet needed."""
    global _silero
    import torch
    if _silero is None:
        torch.set_num_threads(4)
        # TorchScript re-optimises the model on its second call (+2 s on the
        # first answer); unoptimised it is just as fast, 0.05-0.1 s per phrase.
        torch._C._jit_set_profiling_executor(False)
        torch._C._jit_set_profiling_mode(False)
        importer = torch.package.PackageImporter(str(HERE / CONFIG["silero_model"]))
        _silero = importer.load_pickle("tts_models", "model")
        _silero.to("cpu")
    audio = _silero.apply_tts(text=text, speaker=CONFIG["silero_voice"], sample_rate=24000,
                              put_accent=True, put_yo=True).numpy()
    # 24 kHz -> 16 kHz for the intercom
    positions = np.arange(0, len(audio), 1.5)
    resampled = np.interp(positions, np.arange(len(audio)), audio)
    return (np.clip(resampled, -1, 1) * 32767).astype(np.int16)

def _synthesize_edge(text):
    """Edge TTS fallback: online, ~2-3 s per phrase from here."""
    import av
    import edge_tts

    async def fetch():
        mp3 = bytearray()
        async for chunk in edge_tts.Communicate(text, CONFIG["edge_voice"]).stream():
            if chunk["type"] == "audio":
                mp3 += chunk["data"]
        return bytes(mp3)

    mp3 = asyncio.run(fetch())
    container = av.open(io.BytesIO(mp3))
    resampler = av.AudioResampler(format="s16", layout="mono", rate=SAMPLE_RATE)
    pcm = []
    for frame in container.decode(audio=0):
        for out in resampler.resample(frame):
            pcm.append(out.to_ndarray().reshape(-1))
    for out in resampler.resample(None):
        pcm.append(out.to_ndarray().reshape(-1))
    return np.concatenate(pcm).astype(np.int16) if pcm else np.zeros(0, np.int16)

# ---------------------------------------------------------------- dialogue

NUMBER_WORDS = {"одну": 1, "одна": 1, "один": 1, "две": 2, "два": 2, "три": 3, "четыре": 4,
                "пять": 5, "шесть": 6, "семь": 7, "восемь": 8, "девять": 9, "десять": 10,
                "пятнадцать": 15, "двадцать": 20, "тридцать": 30, "сорок": 40,
                "пятьдесят": 50, "полчаса": 30}

def plural(n, one, few, many):
    n = abs(n) % 100
    if 11 <= n <= 14:
        return many
    return one if n % 10 == 1 else few if 2 <= n % 10 <= 4 else many

def quick_command(question):
    """Timer and stop commands without the language model: instant and free."""
    import re
    q = question.lower().replace("ё", "е")
    everywhere = bool(re.search(r"\b(всем|везде|на всех|во всех|все коробки)\b", q))
    if re.search(r"\b(стоп|останови|отмени|выключи)\b.*\b(таймер|будильник|музык|мелоди)", q) \
            or q.strip(" .!") in ("стоп", "хватит", "выключи", "стоп всем"):
        stop_timer(everywhere)
        return "Таймер остановлен везде." if everywhere else "Таймер остановлен."
    if "таймер" not in q and "засеки" not in q:
        return None
    if "полчаса" in q:
        seconds = 1800
    else:
        seconds = 0
        for amount, unit in re.findall(r"(\d+|[а-я]+)\s*(час|мин|сек)", q):
            n = int(amount) if amount.isdigit() else NUMBER_WORDS.get(amount)
            if n is None:
                continue
            seconds += n * {"час": 3600, "мин": 60, "сек": 1}[unit]
        if not seconds and re.search(r"\b(минуту|минутку)\b", q):
            seconds = 60
    if not seconds:
        return None  # let the model figure out unusual phrasing
    set_timer(seconds, everywhere)
    h, rest = divmod(seconds, 3600)
    m, s = divmod(rest, 60)
    parts = []
    if h: parts.append(f"{h} {plural(h, 'час', 'часа', 'часов')}")
    if m: parts.append(f"{m} {plural(m, 'минуту', 'минуты', 'минут')}")
    if s: parts.append(f"{s} {plural(s, 'секунду', 'секунды', 'секунд')}")
    where = " на всех коробках" if everywhere else ""
    return "Таймер на " + " ".join(parts) + f" поставлен{where}."

def degrees(n):
    return f"{n} {plural(n, 'градус', 'градуса', 'градусов')}"

def quick_answer(question):
    """Weather, time and date from templates: about 3 s faster than the model."""
    q = question.lower().replace("ё", "е")
    if "погод" in q or "температур" in q or "градус" in q:
        if any(w in q for w in (" в ", "городе")) and not any(
                w in q for w in ("дома", "у нас", "долгопрудн")):
            return None  # another city: let the model geocode it
        try:
            w = get_weather()
        except Exception as error:
            print("weather error:", error, flush=True)
            return "Не могу узнать погоду: сервис погоды сейчас не отвечает."
        if "завтра" in q:
            d = w["завтра"]
            rain = f", вероятность осадков {d['вероятность_осадков']} процентов" \
                if d["вероятность_осадков"] >= 30 else ""
            return (f"Завтра {d['погода']}, ночью {degrees(d['мин'])}, днём {degrees(d['макс'])}{rain}.")
        c, d = w["сейчас"], w["сегодня"]
        feels = f", ощущается как {c['ощущается']}" if abs(c["ощущается"] - c["температура"]) >= 3 else ""
        rain = f" Вероятность осадков {d['вероятность_осадков']} процентов." \
            if d["вероятность_осадков"] >= 30 else ""
        later = (f"Ночью {degrees(w['ночью'])}." if datetime.now().hour >= 17
                 else f"Днём {degrees(d['макс'])}.")
        return f"Сейчас {degrees(c['температура'])}{feels}, {c['погода']}. {later}{rain}"
    if "который час" in q or "сколько времени" in q or q.startswith("время"):
        return f"Сейчас {datetime.now().strftime('%H:%M')}."
    if "число" in q or "какое сегодня" in q or "день недели" in q or "какой сегодня день" in q:
        t = get_time()
        months = ["января", "февраля", "марта", "апреля", "мая", "июня", "июля", "августа",
                  "сентября", "октября", "ноября", "декабря"]
        now = datetime.now()
        return f"Сегодня {t['день_недели']}, {now.day} {months[now.month - 1]}."
    return None

# Phrases Whisper tends to "hear" in silence or noise (from video subtitles).
HALLUCINATIONS = ("субтитр", "продолжение следует", "спасибо за просмотр", "подписывайтесь",
                  "редактор", "корректор", "до новых встреч", "dimatorzok")
HISTORY_TTL_S = 180     # a conversation remembers the last 3 minutes
HISTORY_TURNS = 4       # ... and at most this many question/answer pairs

def looks_like_noise(text):
    t = text.lower().strip(" .!?…")
    return len(t) < 2 or any(h in t for h in HALLUCINATIONS)

class Assistant:
    def __init__(self):
        self.llm = GigaChat(HERE / CONFIG["gigachat_key_file"])
        self.timing = ""
        self.history = {}   # room -> (last time, [messages])

    def _history(self, room):
        last, messages = self.history.get(room, (0, []))
        return messages if time.time() - last < HISTORY_TTL_S else []

    def _remember(self, room, question, text):
        messages = self._history(room) + [{"role": "user", "content": question},
                                          {"role": "assistant", "content": text}]
        self.history[room] = (time.time(), messages[-2 * HISTORY_TURNS:])

    def answer(self, question, room=0):
        text = self._answer(question, room)
        self._remember(room, question, text)
        return text

    def _answer(self, question, room):
        quick = quick_command(question) or quick_answer(question)
        if quick:
            return quick
        messages = ([{"role": "system", "content": SYSTEM_PROMPT}] + self._history(room) +
                    [{"role": "user", "content": question}])
        for _ in range(3):  # at most a few tool calls per question
            reply = self.llm.chat(messages, TOOLS)
            call = reply.get("function_call")
            if not call:
                return reply.get("content", "").strip()
            name, args = call["name"], call.get("arguments") or {}
            if isinstance(args, str):
                args = json.loads(args or "{}")
            try:
                result = TOOL_FUNCS[name](**args)
            except Exception as error:
                result = {"ошибка": str(error)}
            messages.append({"role": "assistant", "content": "",
                             "function_call": {"name": name, "arguments": args}})
            messages.append({"role": "function", "name": name,
                             "content": json.dumps(result, ensure_ascii=False)})
        return "Не получилось ответить, попробуйте спросить по-другому."

    def handle_audio(self, pcm16, room=0, box_ip=None):
        """Full turn: question audio -> (question text, answer text, audio phrases).

        The audio is a generator: each phrase is synthesized just before it is
        needed, so playback can start after the first one. Nothing recognised
        (silence, noise) gives an empty answer: the box has already beeped."""
        global CURRENT_BOX
        CURRENT_BOX = box_ip   # timer commands go to the box that asked
        started = time.time()
        question = transcribe(pcm16)
        heard = time.time()
        if looks_like_noise(question):
            self.timing = f"речь {heard - started:.1f} с, ничего не сказано"
            return question, "", iter(())
        try:
            text = self.answer(question, room)
        except BoxError as error:
            print("assistant error:", error, flush=True)
            text = "Коробка не ответила, команда не выполнена."
        except requests.RequestException as error:   # GigaChat unreachable
            print("assistant error:", error, flush=True)
            text = "Нет связи с помощником, попробуйте позже."
        except Exception as error:
            print("assistant error:", repr(error), flush=True)
            text = "Что-то пошло не так, попробуйте ещё раз."
        self.timing = f"речь {heard - started:.1f} с, ответ {time.time() - heard:.1f} с"
        return question, text, (synthesize(part) for part in sentences(text))
