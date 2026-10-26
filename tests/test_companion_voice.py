"""Synthetic ASR and creation boundaries. Never calls a provider or account."""
import hashlib
import io
import json
import struct
import unittest
from urllib.error import HTTPError
from test_companion_tasks import CacheFixture,FakeTransport,collection_row,resource_row,multi,PRINCIPAL,COLLECTION,BASE
from companion.tasks.common import packed,TaskError
from companion.tasks.speech import MiMo,wav_seconds,title
from companion.tasks.creation import CreationWorker,calendar
from companion.tasks.service import API
from companion.tasks.caldav import Transport,CalDAV


def wave(size=3200):
    return struct.pack("<4sI4s4sIHHIIHH4sI",b"RIFF",size+36,b"WAVE",b"fmt ",16,1,1,16000,32000,2,16,b"data",size)+bytes(size)


class SpeechTests(unittest.TestCase):
    def test_empty_report_is_only_a_creation_probe_hint(self):
        transport=FakeTransport();transport.reports[COLLECTION]=multi()
        with self.assertRaisesRegex(TaskError,'INVALID_DAV'):
            transport.get_resource(COLLECTION+'new.ics')
        with self.assertRaisesRegex(TaskError,'NOT_FOUND'):
            transport.get_resource(COLLECTION+'new.ics',allow_missing=True)
        # Required post-write/completion reads remain strict after the probe.
        with self.assertRaisesRegex(TaskError,'INVALID_DAV'):
            transport.get_resource(COLLECTION+'new.ics')

    def test_missing_probe_does_not_accept_malformed_or_substituted_resources(self):
        transport=FakeTransport()
        for raw in (b'invalid XML',b'<not-multistatus/>',multi(resource_row(href='https://other.test/new.ics')),
                    multi(resource_row(),resource_row())):
            transport.reports[COLLECTION]=raw
            with self.assertRaises(TaskError) as caught:
                transport.get_resource(COLLECTION+'new.ics',allow_missing=True)
            self.assertNotEqual(caught.exception.code,'NOT_FOUND')

    def test_creation_transport_never_overwrites(self):
        transport=Transport(BASE,"fixture","fixture-password"); captured=[]
        class Opener:
            def open(self,req,timeout):
                captured.append(req); raise HTTPError(req.full_url,412,"private",{},None)
        transport.opener=Opener()
        with self.assertRaisesRegex(TaskError,"CONFLICT"): transport.put_new(COLLECTION+"new.ics",b"calendar")
        self.assertEqual(captured[0].get_header("If-none-match"),"*")
        self.assertIsNone(captured[0].get_header("If-match"))
        with self.assertRaisesRegex(TaskError,"UNSAFE_HREF"): transport.put_new("https://other.test/new.ics",b"calendar")
        self.assertEqual(len(captured),1)

    def test_discovery_requires_owned_bind_privilege_for_creation(self):
        transport=FakeTransport(); client=CalDAV(transport,"account")
        for privilege,expected in (("write-content",False),("bind",True),("write",True),("all",True)):
            extra=f"<d:owner><d:href>{PRINCIPAL}</d:href></d:owner><d:current-user-privilege-set><d:privilege><d:{privilege}/></d:privilege></d:current-user-privilege-set>"
            transport.listing=multi(collection_row().replace("</d:prop>",extra+"</d:prop>"))
            self.assertEqual(client.discover()[0]["creation_allowed"],expected)
        transport.listing=transport.listing.replace(PRINCIPAL.encode(),(PRINCIPAL+"other/").encode())
        self.assertFalse(client.discover()[0]["creation_allowed"])

    def test_wave_envelope_and_limits(self):
        self.assertEqual(wav_seconds(wave()),1)
        self.assertEqual(wav_seconds(wave(384000)),12)
        for raw in (wave(384002),wave(1598),wave()+b"x",wave().replace(b"WAVE",b"MP3!"),wave()[:30]):
            with self.assertRaises(TaskError): wav_seconds(raw)

    def test_titles_reject_empty_control_oversize(self):
        self.assertEqual(title('买牛奶，鸡蛋;"面包"\\'), '买牛奶，鸡蛋;"面包"\\')
        for value in ("", " x", "x\n", "a\x00b", "x"*513, "x\x85y"):
            with self.assertRaises(TaskError): title(value)

    def test_provider_request_and_no_retry(self):
        provider=MiMo("k"*32)
        class Response:
            status=200; headers={}
            def __enter__(self): return self
            def __exit__(self,*args): pass
            def read1(self,n):
                data=self.raw[:n]; self.raw=self.raw[n:]; return data
        class Opener:
            calls=0
            def open(self,req,timeout):
                self.calls+=1; self.req=req
                response=Response(); response.raw=packed({"choices":[{"finish_reason":"stop","message":{"content":"买牛奶"}}]})
                return response
        provider.opener=Opener()
        self.assertEqual(provider.transcribe(wave()),"买牛奶")
        body=json.loads(provider.opener.req.data)
        self.assertEqual((body["model"],body["asr_options"],body["stream"]),("mimo-v2.5-asr",{"language":"zh"},False))
        self.assertEqual(provider.opener.calls,1)
        provider.opener.open=lambda *a,**k: (_ for _ in ()).throw(HTTPError("https://example.invalid",401,"private",{},None))
        with self.assertRaisesRegex(TaskError,"SPEECH_AUTH_FAILED"): provider.transcribe(wave())


class VoiceTests(CacheFixture):
    def setUp(self):
        super().setUp()
        collection=dict(self.collections[0],creation_allowed=True,completion_allowed=True)
        self.store.set_collections([collection]); self.collection=collection
        self.store.publish([],[],1)
        class Speech:
            daily_seconds=2; calls=0
            def transcribe(self,raw): self.calls+=1; return "买牛奶"
        self.speech=Speech(); self.api=API(self.store,"r"*32,"a"*32,"w"*32,self.speech)
        class Resource:
            raw=None; etag='"new"'; puts=0; lost=False; fail=False
            def get_resource(self,href,*,allow_missing=False):
                if self.fail: raise TaskError("UNAVAILABLE")
                if self.raw is None: raise TaskError("NOT_FOUND")
                return self.raw,self.etag
            def put_new(self,href,raw):
                if self.raw is not None: raise TaskError("CONFLICT")
                self.raw=raw; self.puts+=1
                if self.lost: self.lost=False; raise TaskError("UNAVAILABLE")
        self.resource=Resource()
        class Client:
            account_id="account"; display_zone="UTC"; transport=self.resource
            def discover(client): return [self.collection]
        self.worker=CreationWorker(self.api.creations,Client())

    def intent(self,**changes):
        value={"operation_id":"f"*32,"collection_id":self.collection["id"],"summary":"买牛奶;鸡蛋,面包\\"}
        value.update(changes); value["payload_hash"]=hashlib.sha256(packed(value)).hexdigest(); return value

    def request(self,path,method="POST",token="w"*32,raw=b"",content="application/json"):
        status=[]
        env={"REQUEST_METHOD":method,"PATH_INFO":path,"HTTP_AUTHORIZATION":"Bearer "+token,
             "CONTENT_LENGTH":str(len(raw)),"CONTENT_TYPE":content,"wsgi.input":io.BytesIO(raw)}
        result=b"".join(self.api(env,lambda code,headers:status.append(code)))
        return status[0],json.loads(result)

    def test_scopes_no_creation_from_recognition_and_daily_budget(self):
        for token in ("r"*32,"a"*32):
            self.assertEqual(self.request("/suite/v1/task-transcriptions",token=token,raw=wave(),content="audio/wav")[0],"403 Forbidden")
            self.assertEqual(self.request("/suite/v1/task-creations",token=token,raw=packed(self.intent()))[0],"403 Forbidden")
        for _ in range(2):
            status,value=self.request("/suite/v1/task-transcriptions",raw=wave(),content="audio/wav")
            self.assertEqual((status,value["summary"]),("200 OK","买牛奶"))
        self.assertEqual(self.request("/suite/v1/task-transcriptions",raw=wave(),content="audio/wav")[1]["error"],"SPEECH_BUDGET_EXCEEDED")
        self.assertEqual(self.speech.calls,2); self.assertEqual(self.resource.puts,0)
        with self.store.connect() as db: self.assertEqual(db.execute("SELECT COUNT(*) FROM creations").fetchone()[0],0)

    def test_durable_admission_create_and_duplicate(self):
        intent=self.intent()
        self.assertEqual(self.request("/suite/v1/task-creations",raw=packed(intent))[1]["state"],"SERVER_DURABLE")
        self.assertEqual(self.resource.puts,0)
        self.worker.step(); self.assertEqual(self.resource.puts,1)
        self.assertEqual(self.api.creations.accept(intent)["state"],"SUCCEEDED")
        self.assertEqual(self.store.page()["total"],1)
        self.assertEqual(self.store.page()["items"][0]["summary"],intent["summary"])
        self.assertFalse(self.worker.step())
        with self.assertRaisesRegex(TaskError,"OPERATION_CONFLICT"): self.api.creations.accept(self.intent(summary="different"))

    def test_creation_claim_lease_reopen_writes_once(self):
        from companion.tasks.creation import CreationStore
        from companion.tasks.store import Store
        intent=self.intent()
        self.api.creations.accept(intent)
        self.assertIsNotNone(self.worker.store.claim())
        # The worker wrote the conditional resource but died before finish().
        href=self.collection["href"]+"passport-"+intent["operation_id"]+".ics"
        self.resource.put_new(href,calendar(intent,self.now))
        reopened=CreationStore(Store(self.store.path,"account",clock=lambda:self.now))
        self.assertIsNone(reopened.claim())
        self.assertEqual(self.resource.puts,1)
        self.now+=601
        self.worker.store=reopened
        self.worker.step()
        self.assertEqual(self.resource.puts,1)
        self.assertEqual(reopened.accept(intent)["state"],"SUCCEEDED")

    def test_creation_retry_pause_requires_explicit_resume_without_asr(self):
        self.assertEqual(self.request("/suite/v1/task-transcriptions",raw=wave(),content="audio/wav")[0],"200 OK")
        calls=self.speech.calls
        intent=self.intent()
        self.api.creations.accept(intent)
        self.resource.fail=True
        for _ in range(5):
            self.worker.step(); self.now+=40
        self.assertEqual(self.api.creations.accept(intent)["state"],"RETRY_PAUSED")
        self.assertFalse(self.worker.step())
        self.assertEqual((self.resource.puts,self.speech.calls),(0,calls))
        self.assertEqual(self.api.creations.accept(intent,retry=True)["state"],"SERVER_DURABLE")
        self.assertEqual(self.speech.calls,calls)
        self.resource.fail=False
        self.worker.step()
        self.assertEqual(self.api.creations.accept(intent)["state"],"SUCCEEDED")
        self.assertEqual((self.resource.puts,self.speech.calls),(1,calls))

    def test_lost_response_and_reopen_recover_without_duplicate(self):
        from companion.tasks.creation import CreationStore
        from companion.tasks.store import Store
        intent=self.intent(); self.api.creations.accept(intent); self.resource.lost=True; self.worker.step()
        self.now+=1000
        self.worker.store=CreationStore(Store(self.store.path,"account",clock=lambda:self.now))
        self.worker.step()
        self.assertEqual(self.worker.store.accept(intent)["state"],"SUCCEEDED")
        self.assertEqual(self.resource.puts,1)

    def test_creation_empty_multiget_then_conditional_put_and_strict_readback(self):
        self._real_transport_creation(lost=False)

    def test_creation_empty_multiget_and_lost_put_recovers_same_resource(self):
        self._real_transport_creation(lost=True)

    def test_creation_probe_race_preserves_the_existing_resource(self):
        self._real_transport_creation(lost=False,race=True)

    def _real_transport_creation(self,lost,race=False):
        from urllib.error import URLError
        from xml.etree.ElementTree import fromstring
        current=[None,None];puts=[]
        class Response(io.BytesIO):
            def __init__(response,raw,status):
                super().__init__(raw);response.status=status;response.headers={}
        class Opener:
            def open(opener,request,timeout):
                method=request.get_method()
                if method=='REPORT':
                    self.assertEqual(fromstring(request.data).tag,'{urn:ietf:params:xml:ns:caldav}calendar-multiget')
                    if current[0] is None:return Response(multi(),207)
                    return Response(multi(resource_row(raw=current[0],href=current[1],etag='"new"')),207)
                self.assertEqual(method,'PUT')
                self.assertEqual(request.get_header('If-none-match'),'*')
                self.assertIsNone(request.get_header('If-match'))
                self.assertIsNone(current[0]);puts.append(request.data)
                current[:]=[request.data,request.full_url]
                if race:
                    current[0]=request.data.replace(b'NEEDS-ACTION',b'IN-PROCESS')
                    raise HTTPError(request.full_url,412,'precondition',{},None)
                if lost:raise URLError('synthetic lost response')
                return Response(b'',201)
        transport=Transport(BASE,'fixture','fixture-password');transport.opener=Opener()
        self.worker.client.transport=transport
        intent=self.intent();self.api.creations.accept(intent);self.worker.step()
        if lost:
            self.assertEqual(self.api.creations.accept(intent)['state'],'RETRY_WAIT')
            self.now+=1000;self.worker.step()
        self.assertEqual(self.api.creations.accept(intent)['state'],'CONFLICT' if race else 'SUCCEEDED')
        self.assertEqual(len(puts),1)
        self.assertEqual(self.store.page()['total'],0 if race else 1)

    def test_colliding_existing_resource_is_preserved(self):
        intent=self.intent(); self.api.creations.accept(intent); self.resource.raw=b"private existing calendar"
        self.worker.step(); self.assertEqual(self.resource.puts,0)
        self.assertEqual(self.api.creations.accept(intent)["state"],"CONFLICT")

    def test_target_required_before_admission_vs_admitted_same_id(self):
        intent=self.intent()
        self.store.select([],1)
        with self.assertRaisesRegex(TaskError,"CREATION_TARGET_REQUIRED"):
            self.api.creations.accept(intent)
        with self.store.connect() as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM creations").fetchone()[0],0)
        self.store.select([self.collection["id"]],2)
        self.assertEqual(self.api.creations.accept(intent)["state"],"SERVER_DURABLE")
        self.store.select([],3)
        # An already admitted ID returns its original receipt without target lookup.
        receipt=self.api.creations.accept(intent)
        self.assertEqual((receipt["operation_id"],receipt["state"]),("f"*32,"SERVER_DURABLE"))
        with self.store.connect() as db:
            self.assertEqual(db.execute("SELECT COUNT(*) FROM creations").fetchone()[0],1)

    def test_selection_and_permissions_are_checked_again(self):
        intent=self.intent(); self.api.creations.accept(intent)
        self.collection["creation_allowed"]=False; self.worker.step()
        self.assertEqual(self.api.creations.accept(intent)["state"],"READ_ONLY")
        self.assertEqual(self.resource.puts,0)
        self.store.select([],1)
        with self.assertRaisesRegex(TaskError,"CREATION_TARGET_REQUIRED"): self.api.creations.target()

    def test_folded_utf8_and_injection_rejection(self):
        intent=self.intent(summary="中文"*80)
        raw=calendar(intent,self.now)
        self.assertTrue(all(len(line)<=75 for line in raw.split(b"\r\n")))
        for line in raw.split(b"\r\n"): line.decode("utf-8")
        with self.assertRaises(TaskError): self.api.creations.accept(self.intent(summary="x\r\nSTATUS:COMPLETED"))


if __name__=="__main__": unittest.main()
