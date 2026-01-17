import unittest

from config.settings import Settings


class MyTestCase(unittest.TestCase):
    def setUp(self):
        pass

    def tearDown(self):
        pass


    def test_env_file_loading(self):
        settings = Settings()
        self.assertEqual(settings.ENV, "dev")
        self.assertEqual(settings.DEBUG, True)
        self.assertEqual(settings.MQTT_BROKER, "127.0.0.1")


if __name__ == '__main__':
    unittest.main()
