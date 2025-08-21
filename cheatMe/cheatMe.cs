using System;
using System.Threading;

class Program
{
    static void Main(string[] args)
    {
        int maValeur = 100; // Valeur qu'on va chercher/modifier

        Console.WriteLine("Programme cible démarré.");
        Console.WriteLine("Tapez une nouvelle valeur pour la modifier.");
        Console.WriteLine("Si dCheat change la valeur, elle s'affichera automatiquement.\n");

        // Thread pour surveiller la valeur en continu
        new Thread(() =>
        {
            int ancienneValeur = maValeur;
            while (true)
            {
                if (maValeur != ancienneValeur)
                {
                    Console.WriteLine($"[!] Valeur modifiée : {ancienneValeur} → {maValeur}");
                    ancienneValeur = maValeur;
                }
                Thread.Sleep(200); // check toutes les 200ms
            }
        }).Start();

        // Thread principal pour que tu puisses changer manuellement
        while (true)
        {
            string input = Console.ReadLine();
            if (int.TryParse(input, out int newVal))
            {
                maValeur = newVal;
                Console.WriteLine($"Nouvelle valeur définie manuellement : {maValeur}");
            }
        }
    }
}
