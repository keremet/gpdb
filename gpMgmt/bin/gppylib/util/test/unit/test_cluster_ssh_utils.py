#!/usr/bin/env python

import mock
import sys, os, pwd
import unittest
from StringIO import StringIO
from mock import patch, call

try:
    gphome = os.environ.get('GPHOME')
    if not gphome:
        raise Exception("GPHOME not set")
    location = "%s/bin/lib" % gphome
    sys.path.append(location)
    from gppylib.util.ssh_utils import HostList, Session, pxssh
except Exception as e:
    print "PYTHON PATH: %s" % ":".join(sys.path)
    print str(e)
    raise

class SshUtilsTestCase(unittest.TestCase):

    def test00_test_filterMultiHomedHosts(self):
        """ 
            filterMultiHomedHosts should deduplicate hostnames
        """

        hostlist = HostList()
        hostlist.add('localhost')
        hostlist.add('localhost')
        hostlist.add('localhost')
        hostlist.filterMultiHomedHosts()
        self.assertEqual(len(hostlist.get()), 1,
                         "There should be only 1 host in the hostlist after calling filterMultiHomedHosts")

    def test01_test_SessionLogin(self):
        """ 
            Session.login test, one success and one failure
        """

        uname = pwd.getpwuid(os.getuid()).pw_name

        s1 = Session()
        s1.login(['localhost', 'fakehost'], uname)
        pxssh_hosts = [pxssh_session.x_peer for pxssh_session in s1.pxssh_list]
        self.assertEqual(pxssh_hosts, ['localhost'])

        s2 = Session()
        try:
            s2.login(['localhost', 'example.com'], uname, all_hosts=True)
            self.assert_("Unrechable")
        except RuntimeError:
            pxssh_hosts = [pxssh_session.x_peer for pxssh_session in s2.pxssh_list]
            self.assertEqual(pxssh_hosts, ['localhost'])

    def test02_pxssh_delaybeforesend(self):
        '''
        test that delaybeforesend is changed properly
        '''
        p1 = pxssh.pxssh()
        self.assertEquals(p1.delaybeforesend, 0.05)

        p2 = pxssh.pxssh(delaybeforesend=3.0,
                        options={"StrictHostKeyChecking": "no",
                                 "BatchMode": "yes"})
        self.assertEquals(p2.delaybeforesend, 3.0)

    def test03_pxssh_sync_multiplier(self):
        '''
        test that sync_multiplier is changed properly
        '''
        with mock.patch.object(pxssh.pxssh, 'login', return_value=None) as mock_login:
            session1 = Session()
            session1.login(['localhost'], 'gpadmin', 0.05, 1.0)
            mock_login.assert_called_with('localhost', 'gpadmin', sync_multiplier=1.0)

            session2 = Session()
            session2.login(['localhost'], 'gpadmin', 1.0, 4.0)
            mock_login.assert_called_with('localhost', 'gpadmin', sync_multiplier=4.0)

    @patch('sys.stdout', new_callable=StringIO)
    def test04_exceptions(self, mock_stdout):
        '''
        Test pxssh.login() exceptions
        '''
        with mock.patch.object(pxssh.pxssh, 'login', side_effect=pxssh.ExceptionPxssh('foo')) as mock_login:
            session1 = Session()
            session1.login(['localhost'], 'gpadmin', 0.05, 1.0)
            self.assertEqual(mock_stdout.getvalue(), '[ERROR] unable to login to localhost\nfoo\n')
            mock_stdout.truncate(0)

        with mock.patch.object(pxssh.pxssh, 'login', side_effect=pxssh.EOF('foo')) as mock_login:
            session2 = Session()
            session2.login(['localhost'], 'gpadmin', 0.05, 1.0)
            self.assertEqual(mock_stdout.getvalue(), '[ERROR] unable to login to localhost\nCould not acquire connection.\nfoo\n')
            mock_stdout.truncate(0)

        with mock.patch.object(pxssh.pxssh, 'login', side_effect=Exception('foo')) as mock_login:
            session2 = Session()
            session2.login(['localhost'], 'gpadmin', 0.05, 1.0)
            self.assertEqual(mock_stdout.getvalue(), '[ERROR] unable to login to localhost\nhint: use gpssh-exkeys to setup public-key authentication between hosts\n')

    @patch('os.getenv', return_value="term")
    @patch('os.putenv')
    @patch('sys.stdout', new_callable=StringIO)
    def test05_login_retry_when_term_variable_is_set(self, mock_stdout, mock_putenv, mock_getenv):
        '''
        Test pxssh.login() retry when there is an exception and TERM env variable is set
        '''

        with mock.patch.object(pxssh.pxssh, 'login', side_effect=pxssh.EOF('foo')) as mock_login:
            session = Session()
            session.login(['localhost'], 'gpadmin', 0.05, 1.0)
            self.assertIn('[ERROR] unable to login to localhost\nCould not acquire connection.\n', mock_stdout.getvalue())
            mock_stdout.truncate(0)
            assert mock_putenv.call_count == 3
            mock_putenv.assert_has_calls([call('TERM', ''), call('TERM', 'term'), call('TERM', 'term')])

    @patch('os.getenv', return_value=None)
    @patch('os.putenv')
    @patch('sys.stdout', new_callable=StringIO)
    def test06_login_does_not_retry_when_term_variable_is_not_set(self, mock_stdout, mock_putenv, mock_getenv):
        '''
        Test pxssh.login() does not retry when there is an exception and TERM env variable is not set
        '''

        with mock.patch.object(pxssh.pxssh, 'login', side_effect=pxssh.EOF('foo')) as mock_login:
            session = Session()
            session.login(['localhost'], 'gpadmin', 0.05, 1.0)
            self.assertIn('[ERROR] unable to login to localhost\nCould not acquire connection.\n', mock_stdout.getvalue())
            self.assertNotIn('Retrying by restoring the TERM env variable.\n', mock_stdout.getvalue())
            mock_stdout.truncate(0)
            mock_putenv.assert_called_once_with('TERM', '')

if __name__ == "__main__":
    unittest.main()
