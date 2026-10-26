"""Synthetic conditional completion and crash/retry boundaries; no account writes."""
import hashlib
import io
import json
import unittest
from urllib.error import HTTPError

from test_companion_tasks import CacheFixture, COLLECTION, BASE, FakeTransport, ics, multi, resource_row
from companion.tasks.common import TaskError, packed
from companion.tasks.completion import CompletionWorker, completed_calendar
from companion.tasks.model import parse_resource
from companion.tasks.service import API
from companion.tasks.caldav import Transport


class Resource:
    def __init__(self, raw):
        self.raw,self.etag=raw,'"base"'
        self.puts=0; self.lost=False; self.conflict=False; self.offline=False

    def get_resource(self, href):
        if self.offline: raise TaskError("UNAVAILABLE")
        return self.raw,self.etag

    def put_existing(self, href, raw, etag):
        if self.conflict or etag!=self.etag: raise TaskError("CONFLICT")
        self.puts+=1; self.raw=raw; self.etag='"changed"'
        if self.lost: self.lost=False; raise TaskError("UNAVAILABLE")
        return b"",self.etag


class CompletionTests(CacheFixture):
    def setUp(self):
        super().setUp()
        raw=ics("SUMMARY:Fixture\r\nDESCRIPTION:keep\\ntext\r\nX-EXTRA:keep\r\n folded\r\n"
                "BEGIN:VALARM\r\nACTION:DISPLAY\r\nDESCRIPTION:keep alarm\r\nTRIGGER:-PT5M\r\nEND:VALARM\r\n")
        self.record=parse_resource(raw,account_id="account",collection_id=self.collections[0]["id"],
                                  href=COLLECTION+"one.ics",etag='"base"',display_zone="UTC")[0]
        self.record["completion_allowed"]=True
        self.store.publish([self.record],[(self.record["href"],'"base"',raw)],1)
        self.resource=Resource(raw)
        class Client:
            display_zone="UTC"
            transport=self.resource
            def discover(client): return [dict(self.collections[0],completion_allowed=True)]
        self.worker=CompletionWorker(self.store,Client())
        self.api=API(self.store,"r"*32,"a"*32,"w"*32)

    def intent(self, **changes):
        value={"operation_id":"1"*32,"task_id":self.record["task_id"],"base_etag":'"base"',
               "desired":"completed","created_at":None,"device_sequence":1}
        value.update(changes)
        value["payload_hash"]=hashlib.sha256(packed(value)).hexdigest()
        return value

    def request(self, method, path, token="w"*32, body=None):
        raw=packed(body) if body is not None else b""; status=[]
        env={"REQUEST_METHOD":method,"PATH_INFO":path,"QUERY_STRING":"",
             "HTTP_AUTHORIZATION":"Bearer "+token,"CONTENT_LENGTH":str(len(raw)),"wsgi.input":io.BytesIO(raw)}
        result=b"".join(self.api(env,lambda code,headers:status.append(code)))
        return status[0],json.loads(result)

    def test_separate_scope_and_durable_admission(self):
        for token in ("r"*32,"a"*32):
            self.assertEqual(self.request("POST","/suite/v1/task-completions",token,self.intent())[0],"403 Forbidden")
        self.assertEqual(self.request("GET","/suite/v1/tasks/snapshot")[0],"403 Forbidden")
        status,body=self.request("POST","/suite/v1/task-completions",body=self.intent())
        self.assertEqual((status,body["state"]),("202 Accepted","SERVER_DURABLE"))
        self.assertEqual(self.resource.puts,0)
        self.assertEqual(self.store.operation("1"*32)["state"],"SERVER_DURABLE")

    def test_completion_preserves_other_text_and_removes_active_item(self):
        before=self.resource.raw
        self.store.accept_completion(self.intent())
        self.assertTrue(self.worker.step())
        self.assertEqual(self.resource.puts,1)
        self.assertEqual(self.store.operation("1"*32)["state"],"SUCCEEDED")
        self.assertEqual(self.store.page()["total"],0)
        for kept in (b"DESCRIPTION:keep\\ntext\r\n",b"X-EXTRA:keep\r\n folded\r\n",b"BEGIN:VALARM\r\nACTION:DISPLAY\r\nDESCRIPTION:keep alarm\r\nTRIGGER:-PT5M\r\nEND:VALARM\r\n"):
            self.assertIn(kept,before); self.assertIn(kept,self.resource.raw)
        parsed=parse_resource(self.resource.raw,account_id="account",collection_id=self.record["collection_id"],
                              href=self.record["href"],etag=self.resource.etag,display_zone="UTC")[0]
        self.assertEqual((parsed["status"],parsed["percent_complete"],parsed["completed_at"]["kind"]),("COMPLETED",100,"utc"))
        self.assertEqual(self.store.state()["downloaded"],100000)
        self.assertEqual(len(json.loads(self.store.state()["snapshot"])),1)

    def test_idempotent_retry_and_hash_collision(self):
        intent=self.intent(); self.store.accept_completion(intent); self.worker.step()
        self.assertEqual(self.store.accept_completion(intent)["state"],"SUCCEEDED")
        self.assertFalse(self.worker.step()); self.assertEqual(self.resource.puts,1)
        with self.assertRaisesRegex(TaskError,"OPERATION_CONFLICT"):
            self.store.accept_completion(self.intent(created_at=123))

    def test_changed_etag_never_puts(self):
        self.store.accept_completion(self.intent()); self.resource.etag='"newer"'
        self.worker.step()
        self.assertEqual(self.resource.puts,0)
        self.assertEqual(self.store.operation("1"*32)["state"],"CONFLICT")
        self.assertEqual(self.store.page()["total"],1)

    def test_conditional_put_race_is_conflict(self):
        self.store.accept_completion(self.intent()); self.resource.conflict=True
        self.worker.step()
        self.assertEqual(self.resource.puts,0)
        self.assertEqual(self.store.operation("1"*32)["state"],"CONFLICT")

    def test_lost_put_response_recovers_without_another_write(self):
        self.store.accept_completion(self.intent()); self.resource.lost=True
        self.worker.step()
        self.assertEqual(self.store.operation("1"*32)["state"],"RETRY_WAIT")
        self.now+=10; self.worker.step()
        self.assertEqual(self.store.operation("1"*32)["state"],"SUCCEEDED")
        self.assertEqual(self.resource.puts,1)

    def test_newer_edit_after_lost_response_is_conflict(self):
        self.store.accept_completion(self.intent()); self.resource.lost=True; self.worker.step()
        self.resource.raw=self.resource.raw.replace(b"SUMMARY:Fixture",b"SUMMARY:Newer edit")
        self.now+=10; self.worker.step()
        self.assertEqual(self.store.operation("1"*32)["state"],"CONFLICT")
        self.assertEqual(self.resource.puts,1)

    def test_server_reopen_and_lease_recovery(self):
        from companion.tasks.store import Store
        self.store.accept_completion(self.intent())
        self.assertIsNotNone(self.store.claim_completion())
        reopened=Store(self.store.path,"account",clock=lambda:self.now)
        self.assertIsNone(reopened.claim_completion())
        self.now+=601
        self.worker.store=reopened; self.worker.step()
        self.assertEqual(reopened.operation("1"*32)["state"],"SUCCEEDED")

    def test_retry_budget_requires_explicit_resume(self):
        self.store.accept_completion(self.intent()); self.resource.offline=True
        for _ in range(5): self.worker.step(); self.now+=1000
        self.assertEqual(self.store.operation("1"*32)["state"],"RETRY_PAUSED")
        self.assertFalse(self.worker.step())
        self.store.accept_completion(self.intent(),retry=True); self.resource.offline=False
        self.worker.step(); self.assertEqual(self.store.operation("1"*32)["state"],"SUCCEEDED")

    def test_readonly_and_recurring_rejected(self):
        with self.store.connect() as db:
            items=json.loads(self.store.state()["snapshot"])
            items[0]["completion_allowed"]=False
            db.execute("UPDATE state SET snapshot=?",(packed(items).decode(),))
        with self.assertRaisesRegex(TaskError,"READ_ONLY"): self.store.accept_completion(self.intent())
        raw=ics("RRULE:FREQ=DAILY\r\n")
        with self.assertRaisesRegex(TaskError,"READ_ONLY"): completed_calendar(raw,self.record,self.now)

    def test_revoked_live_rights_and_auth_halt_do_not_write(self):
        self.store.accept_completion(self.intent())
        self.worker.client.discover=lambda: [dict(self.collections[0],completion_allowed=False)]
        self.worker.step()
        self.assertEqual(self.store.operation("1"*32)["state"],"READ_ONLY")
        self.assertEqual(self.resource.puts,0)
        other=self.intent(operation_id="2"*32)
        self.store.accept_completion(other)
        self.store.failure("AUTH_FAILED")
        self.worker.step()
        self.assertEqual(self.store.operation("2"*32)["state"],"AUTH_FAILED")
        self.assertEqual(self.resource.puts,0)

    def test_completion_uses_report_despite_stale_get_cache(self):
        self._report_completion(lost=False)

    def test_report_line_endings_recover_lost_put_response(self):
        self._report_completion(lost=True)

    def _report_completion(self,lost):
        from urllib.error import URLError
        from xml.etree.ElementTree import fromstring
        original=self.resource.raw; current=[original,'"base"']; calls=[]; puts=[]
        class Response(io.BytesIO):
            def __init__(response,raw,status,headers=None):
                super().__init__(raw);response.status=status;response.headers=headers or {}
        class Opener:
            def open(opener,request,timeout):
                method=request.get_method();calls.append(method)
                if method=='GET':
                    return Response(original,200,{'ETag':'"base"','Age':'600'})
                if method=='REPORT':
                    self.assertEqual(request.get_header('Depth'),'0')
                    self.assertEqual(request.full_url,COLLECTION)
                    query=fromstring(request.data)
                    self.assertEqual(query.tag,'{urn:ietf:params:xml:ns:caldav}calendar-multiget')
                    return Response(multi(resource_row(raw=current[0],etag=current[1])),207)
                self.assertEqual(method,'PUT');self.assertEqual(request.get_header('If-match'),current[1])
                puts.append(request.data);current[:]=[request.data,'"changed"']
                if lost:raise URLError('synthetic lost PUT response')
                return Response(b'',204)
        transport=Transport(BASE,'fixture','fixture-password');transport.opener=Opener()
        self.worker.client.transport=transport
        self.store.accept_completion(self.intent());self.worker.step()
        if lost:
            self.assertEqual(self.store.operation('1'*32)['state'],'RETRY_WAIT')
            self.now+=10;self.worker.step()
        self.assertEqual(self.store.operation('1'*32)['state'],'SUCCEEDED')
        self.assertEqual(len(puts),1);self.assertNotIn('GET',calls)
        self.assertEqual(self.store.page()['total'],0)


class CompletionTransportTests(unittest.TestCase):
    def test_multiget_requires_exact_single_resource(self):
        transport=FakeTransport()
        for payload,code in ((multi(resource_row(),resource_row()),'INVALID_DAV'),
                (multi(resource_row(href='https://other.test/one.ics')),'UNSAFE_HREF'),
                (multi('<d:response><d:href>'+COLLECTION+'one.ics</d:href><d:status>HTTP/1.1 404 Not Found</d:status></d:response>'),'NOT_FOUND')):
            transport.reports[COLLECTION]=payload
            with self.subTest(code=code),self.assertRaisesRegex(TaskError,code):
                transport.get_resource(COLLECTION+'one.ics')

    def test_put_requires_if_match_and_keeps_redirect_guard(self):
        transport=Transport("https://example.test","fixture","fixture-password")
        captured=[]
        class Opener:
            def open(self,request,timeout):
                captured.append(request)
                raise HTTPError(request.full_url,412,"precondition",{},None)
        transport.opener=Opener()
        with self.assertRaisesRegex(TaskError,"CONFLICT"):
            transport.put_existing(transport.dav_url+"fixture.ics",b"calendar",'"base"')
        self.assertEqual(len(captured),1)
        self.assertEqual(captured[0].get_method(),"PUT")
        self.assertEqual(captured[0].get_header("If-match"),'"base"')
        for etag in ("*",'W/"base"','"bad\r\n"'):
            with self.assertRaisesRegex(TaskError,"INVALID_OPERATION"):
                transport.put_existing(transport.dav_url+"fixture.ics",b"calendar",etag)
        with self.assertRaisesRegex(TaskError,"UNSAFE_HREF"):
            transport.put_existing("https://other.test/remote.php/dav/fixture.ics",b"calendar",'"base"')
        self.assertEqual(len(captured),1)


if __name__=="__main__": unittest.main()
